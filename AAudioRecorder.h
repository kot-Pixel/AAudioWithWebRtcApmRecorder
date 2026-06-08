//
// Created by kotlinx on 2025/12/7.
//

#ifndef AAUDIORECORDER_AAUDIORECORDER_H
#define AAUDIORECORDER_AAUDIORECORDER_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#include <fstream>
#include <cstring>

#include <aaudio/AAudio.h>
#include <android/log.h>
#include "modules/audio_processing/include/audio_processing.h"
#include "lwrb.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "NDKRecorder", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "NDKRecorder", __VA_ARGS__)

// 10 帧 = 100ms
#define BUFFER_SIZE (480 * sizeof(int16_t) * 10)

class CallbackPCMRecorder {
public:
    CallbackPCMRecorder()
        : inputStream(nullptr),
          inputBuilder(nullptr),
          outputStream(nullptr),
          outputBuilder(nullptr),
          streamCfg_(SAMPLE_RATE, CHANNELS) {
        lwrb_init(&audio_rb, audio_rb_data, BUFFER_SIZE);
    }

    // 启动顺序：
    // loadFarPcm → initApm → openOutputStream（含 output callback，APM 已就绪）
    // → openInputStream → 启动 handler → 等 handler 就绪 → startInputStream
    bool start(const char* farPath, int streamDelayMs) {
        streamDelayMs_ = streamDelayMs;

        if (!loadFarPcm(farPath))    return false;
        if (!initApm())              return false;
        if (!openOutputStream())     return false;
        if (!openInputStream())      return false;

        running = true;
        handlerThread = std::thread(&CallbackPCMRecorder::handlerLoop, this);

        {
            std::unique_lock<std::mutex> lock(startMutex);
            startCv.wait(lock, [this] { return handlerReady.load(); });
        }

        if (!startInputStream()) {
            running = false;
            rb_cv.notify_all();
            if (handlerThread.joinable()) handlerThread.join();
            closeStream(inputStream,  inputBuilder);
            closeStream(outputStream, outputBuilder);
            return false;
        }

        LOGI("AEC ready: far_frames=%zu delay=%d ms buffer=%d ms",
             farPcm.size() / FRAME_LEN,
             streamDelayMs_,
             static_cast<int>(BUFFER_SIZE / sizeof(int16_t) * 1000 / SAMPLE_RATE));
        return true;
    }

    void stop() {
        running = false;
        rb_cv.notify_all();

        if (handlerThread.joinable()) handlerThread.join();

        closeStream(inputStream,  inputBuilder);
        closeStream(outputStream, outputBuilder);
        inputStream  = nullptr; inputBuilder  = nullptr;
        outputStream = nullptr; outputBuilder = nullptr;

        LOGI("Stopped: input_callbacks=%llu dropped_frames=%llu",
             (unsigned long long)inputCallbackCount.load(),
             (unsigned long long)droppedFramesTotal.load());
    }

    ~CallbackPCMRecorder() { stop(); }

private:
    static constexpr int    SAMPLE_RATE = 48000;
    static constexpr int    CHANNELS    = 1;
    static constexpr size_t FRAME_LEN   = 480;   // 10ms @ 48kHz
    static constexpr uint64_t kCbLogInterval = 100;

    AAudioStream*        inputStream;
    AAudioStreamBuilder* inputBuilder;
    AAudioStream*        outputStream;
    AAudioStreamBuilder* outputBuilder;

    rtc::scoped_refptr<webrtc::AudioProcessing> apm;
    webrtc::StreamConfig streamCfg_;   // shared by output cb + handler

    lwrb_t   audio_rb;
    uint8_t  audio_rb_data[BUFFER_SIZE];

    std::vector<int16_t> farPcm;
    size_t farOffset = 0;   // 仅 output callback 访问，无需锁
    int    streamDelayMs_ = 100;

    std::atomic<bool>     running{false};
    std::atomic<bool>     handlerReady{false};
    std::atomic<uint64_t> inputCallbackCount{0};
    std::atomic<uint64_t> droppedFramesTotal{0};

    std::thread             handlerThread;
    std::mutex              captureMutex;
    std::condition_variable rb_cv;
    std::mutex              startMutex;
    std::condition_variable startCv;

    // ------------------------------------------------------------------ //
    //  初始化
    // ------------------------------------------------------------------ //

    bool loadFarPcm(const char* farPath) {
        std::ifstream f(farPath, std::ios::binary | std::ios::ate);
        if (!f.is_open()) { LOGE("Cannot open far PCM: %s", farPath); return false; }

        const std::streamsize bytes = f.tellg();
        if (bytes <= 0 || bytes % sizeof(int16_t) != 0) {
            LOGE("Invalid far PCM size"); return false;
        }

        f.seekg(0);
        farPcm.resize(static_cast<size_t>(bytes / sizeof(int16_t)));
        f.read(reinterpret_cast<char*>(farPcm.data()), bytes);
        if (!f || farPcm.size() < FRAME_LEN) {
            LOGE("Failed to read far PCM"); farPcm.clear(); return false;
        }
        farOffset = 0;
        LOGI("Loaded far PCM: %zu samples (%.1f s)",
             farPcm.size(), farPcm.size() / (float)SAMPLE_RATE);
        return true;
    }

    static const char* nsLevelName(
        webrtc::AudioProcessing::Config::NoiseSuppression::Level level
    ) {
        switch (level) {
            case webrtc::AudioProcessing::Config::NoiseSuppression::kLow:      return "Low";
            case webrtc::AudioProcessing::Config::NoiseSuppression::kModerate: return "Moderate";
            case webrtc::AudioProcessing::Config::NoiseSuppression::kHigh:     return "High";
            case webrtc::AudioProcessing::Config::NoiseSuppression::kVeryHigh: return "VeryHigh";
        }
        return "Unknown";
    }

    bool initApm() {
        webrtc::AudioProcessing::Config cfg;

        // AEC3（无强度等级，始终全力运行）
        cfg.echo_canceller.enabled                = true;
        cfg.echo_canceller.enforce_high_pass_filtering = true;  // 默认 true，去除低频回声

        // HPF：去除直流和低频干扰，有利于 AEC
        cfg.high_pass_filter.enabled              = true;

        // NS 辅助清除 AEC 残留余回声，基于 AEC 线性输出分析，效果最佳
        cfg.noise_suppression.enabled             = true;
        cfg.noise_suppression.level               =
            webrtc::AudioProcessing::Config::NoiseSuppression::kVeryHigh;
        cfg.noise_suppression.analyze_linear_aec_output_when_available = true;

        cfg.gain_controller2.enabled              = false;

        webrtc::AudioProcessingBuilder b;
        b.SetConfig(cfg);
        apm = b.Create();
        if (!apm) { LOGE("Failed to create APM"); return false; }

        LOGI("WebRTC APM created: AEC3=%s mobile=%s enforce_hpf=%s | HPF=%s | NS=%s level=%s analyze_aec=%s | AGC2=%s",
             cfg.echo_canceller.enabled ? "on" : "off",
             cfg.echo_canceller.mobile_mode ? "on" : "off",
             cfg.echo_canceller.enforce_high_pass_filtering ? "on" : "off",
             cfg.high_pass_filter.enabled ? "on" : "off",
             cfg.noise_suppression.enabled ? "on" : "off",
             nsLevelName(cfg.noise_suppression.level),
             cfg.noise_suppression.analyze_linear_aec_output_when_available ? "on" : "off",
             cfg.gain_controller2.enabled ? "on" : "off");
        return true;
    }

    bool openOutputStream() {
        aaudio_result_t r = AAudio_createStreamBuilder(&outputBuilder);
        if (r != AAUDIO_OK) { LOGE("Cannot create output builder"); return false; }

        AAudioStreamBuilder_setDirection(outputBuilder,     AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setSampleRate(outputBuilder,    SAMPLE_RATE);
        AAudioStreamBuilder_setChannelCount(outputBuilder,  CHANNELS);
        AAudioStreamBuilder_setFormat(outputBuilder,        AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setSharingMode(outputBuilder,   AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setPerformanceMode(outputBuilder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setUsage(outputBuilder,         AAUDIO_USAGE_MEDIA);
        AAudioStreamBuilder_setContentType(outputBuilder,   AAUDIO_CONTENT_TYPE_SPEECH);
        // output 用 callback 驱动，解耦播放，不再 blocking write
        AAudioStreamBuilder_setDataCallback(outputBuilder,  outputCallback, this);
        AAudioStreamBuilder_setErrorCallback(outputBuilder, errorCallback,  this);

        r = AAudioStreamBuilder_openStream(outputBuilder, &outputStream);
        if (r != AAUDIO_OK) { LOGE("Cannot open output stream"); return false; }

        r = AAudioStream_requestStart(outputStream);
        if (r != AAUDIO_OK) { LOGE("Cannot start output stream"); return false; }

        LOGI("AAudio output started: rate=%d", AAudioStream_getSampleRate(outputStream));
        return true;
    }

    bool openInputStream() {
        aaudio_result_t r = AAudio_createStreamBuilder(&inputBuilder);
        if (r != AAUDIO_OK) { LOGE("Cannot create input builder"); return false; }

        AAudioStreamBuilder_setDirection(inputBuilder,      AAUDIO_DIRECTION_INPUT);
        AAudioStreamBuilder_setSampleRate(inputBuilder,     SAMPLE_RATE);
        AAudioStreamBuilder_setChannelCount(inputBuilder,   CHANNELS);
        AAudioStreamBuilder_setFormat(inputBuilder,         AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setSharingMode(inputBuilder,    AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setPerformanceMode(inputBuilder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setDataCallback(inputBuilder,   inputCallback, this);
        AAudioStreamBuilder_setErrorCallback(inputBuilder,  errorCallback, this);

        r = AAudioStreamBuilder_openStream(inputBuilder, &inputStream);
        if (r != AAUDIO_OK) { LOGE("Cannot open input stream"); return false; }

        const int sr = AAudioStream_getSampleRate(inputStream);
        LOGI("AAudio input opened (not started): actual_rate=%d", sr);
        if (sr != SAMPLE_RATE) { LOGE("Input rate mismatch"); return false; }
        return true;
    }

    bool startInputStream() {
        aaudio_result_t r = AAudioStream_requestStart(inputStream);
        if (r != AAUDIO_OK) { LOGE("Cannot start input stream"); return false; }
        LOGI("AAudio input started");
        return true;
    }

    // ------------------------------------------------------------------ //
    //  far-end 内存读取（仅 output callback 线程访问）
    // ------------------------------------------------------------------ //

    void readFarSamples(int16_t* dst, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            dst[i] = farPcm[farOffset++];
            if (farOffset >= farPcm.size()) farOffset = 0;
        }
    }

    // ------------------------------------------------------------------ //
    //  Output callback（AAudio 音频线程，约每 ~6-10ms 触发）
    //  职责：读 far_pcm → ProcessReverseStream → 写扬声器
    //  不再占用 handler 线程，和 ProcessStream 完全解耦
    // ------------------------------------------------------------------ //

    static aaudio_data_callback_result_t outputCallback(
        AAudioStream* /*stream*/,
        void*    userData,
        void*    audioData,
        int32_t  numFrames
    ) {
        auto*    rec = static_cast<CallbackPCMRecorder*>(userData);
        auto*    out = static_cast<int16_t*>(audioData);

        int32_t  processed = 0;

        // 以 FRAME_LEN 为单位处理（WebRTC 要求整帧）
        while (processed + static_cast<int32_t>(FRAME_LEN) <= numFrames) {
            // 从内存读 160 个 int16 样本送到扬声器 buffer
            rec->readFarSamples(out + processed, FRAME_LEN);

            // 同时送给 AEC 作为参考信号
            float far_f[FRAME_LEN];
            for (size_t i = 0; i < FRAME_LEN; ++i)
                far_f[i] = out[processed + i] / 32767.0f;

            const float* revIn[1]  = {far_f};
            float        revOutBuf[FRAME_LEN];
            float*       revOut[1] = {revOutBuf};

            rec->apm->ProcessReverseStream(
                revIn,
                rec->streamCfg_,
                rec->streamCfg_,
                revOut
            );

            processed += static_cast<int32_t>(FRAME_LEN);
        }

        // 剩余不足一帧的部分：只播放，不送 APM（极少出现）
        if (processed < numFrames) {
            rec->readFarSamples(out + processed,
                                static_cast<size_t>(numFrames - processed));
        }

        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    // ------------------------------------------------------------------ //
    //  Input callback（AAudio 音频线程）
    //  职责：mic 数据 → ring buffer
    // ------------------------------------------------------------------ //

    static aaudio_data_callback_result_t inputCallback(
        AAudioStream* /*stream*/,
        void*    userData,
        void*    audioData,
        int32_t  numFrames
    ) {
        auto* rec = static_cast<CallbackPCMRecorder*>(userData);
        auto* in  = static_cast<int16_t*>(audioData);

        const uint64_t idx      = ++rec->inputCallbackCount;
        const size_t   freeS    = lwrb_get_free(&rec->audio_rb) / sizeof(int16_t);
        const size_t   fullS    = lwrb_get_full(&rec->audio_rb) / sizeof(int16_t);
        const size_t   toWrite  = std::min(static_cast<size_t>(numFrames), freeS);
        const size_t   dropped  = static_cast<size_t>(numFrames) - toWrite;

        if (dropped > 0) {
            rec->droppedFramesTotal += dropped;
            LOGE("Ring buffer overflow, dropping %zu frames (total=%llu)",
                 dropped, (unsigned long long)rec->droppedFramesTotal.load());
        }

        if (idx == 1 || idx % kCbLogInterval == 0) {
            LOGI("input cb #%llu: got=%d wrote=%zu free=%zu full=%zu drop_total=%llu",
                 (unsigned long long)idx, numFrames, toWrite, freeS, fullS,
                 (unsigned long long)rec->droppedFramesTotal.load());
        }

        if (toWrite > 0) {
            lwrb_write(&rec->audio_rb,
                       reinterpret_cast<uint8_t*>(in),
                       toWrite * sizeof(int16_t));
            rec->rb_cv.notify_one();
        }

        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    // ------------------------------------------------------------------ //
    //  Handler thread
    //  职责：从 ring buffer 读 mic 数据 → ProcessStream（AEC 近端）
    //  不再参与播放，和 output callback 完全解耦
    // ------------------------------------------------------------------ //

    void handlerLoop() {
        {
            std::lock_guard<std::mutex> lk(startMutex);
            handlerReady = true;
        }
        startCv.notify_one();
        LOGI("Handler thread ready");

        const size_t bytes = FRAME_LEN * sizeof(int16_t);

        std::vector<int16_t> near_pcm(FRAME_LEN);
        std::vector<float>   near_f(FRAME_LEN);
        std::vector<float>   out_f(FRAME_LEN);

        uint64_t frameCount   = 0;
        double   nearMsTotal  = 0.0;
        double   totalMsTotal = 0.0;

        while (running) {
            const auto t0 = std::chrono::steady_clock::now();

            // 等待 ring buffer 有足够数据
            {
                std::unique_lock<std::mutex> lk(captureMutex);
                rb_cv.wait(lk, [&] {
                    return !running || lwrb_get_full(&audio_rb) >= bytes;
                });
            }
            if (!running) break;

            const size_t got = lwrb_read(
                &audio_rb,
                reinterpret_cast<uint8_t*>(near_pcm.data()),
                bytes
            );
            if (got != bytes) continue;

            // int16 → float
            for (size_t i = 0; i < FRAME_LEN; ++i)
                near_f[i] = near_pcm[i] / 32767.0f;

            // AEC 近端处理
            apm->set_stream_delay_ms(streamDelayMs_);

            float* nearIn[1]  = {near_f.data()};
            float* nearOut[1] = {out_f.data()};

            const auto tNear0 = std::chrono::steady_clock::now();
            const int  rc     = apm->ProcessStream(
                nearIn, streamCfg_, streamCfg_, nearOut
            );
            const auto tNear1 = std::chrono::steady_clock::now();

            if (rc != 0) {
                LOGE("ProcessStream failed: %d", rc);
                continue;
            }

            const auto t1 = std::chrono::steady_clock::now();
            nearMsTotal  += std::chrono::duration<double, std::milli>(tNear1 - tNear0).count();
            totalMsTotal += std::chrono::duration<double, std::milli>(t1    - t0    ).count();
            ++frameCount;

            if (frameCount % 100 == 0) {
                LOGI("CPU per-frame: near=%.3f ms  total=%.3f ms  delay=%d  rb_full=%zu",
                     nearMsTotal  / 100.0,
                     totalMsTotal / 100.0,
                     streamDelayMs_,
                     lwrb_get_full(&audio_rb) / sizeof(int16_t));
                nearMsTotal  = 0.0;
                totalMsTotal = 0.0;
            }
        }
    }

    // ------------------------------------------------------------------ //
    //  Error callback & 公共工具
    // ------------------------------------------------------------------ //

    static void errorCallback(
        AAudioStream* /*stream*/,
        void*         /*userData*/,
        aaudio_result_t error
    ) {
        LOGE("AAudio error: %d (%s)", error, AAudio_convertResultToText(error));
    }

    static void closeStream(AAudioStream*& stream, AAudioStreamBuilder*& builder) {
        if (stream)  { AAudioStream_requestStop(stream); AAudioStream_close(stream); stream = nullptr; }
        if (builder) { AAudioStreamBuilder_delete(builder); builder = nullptr; }
    }
};

#endif //AAUDIORECORDER_AAUDIORECORDER_H

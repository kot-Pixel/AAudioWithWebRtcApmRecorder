//
// Created by kotlinx on 2025/12/7.
//

#ifndef AAUDIORECORDER_AAUDIORECORDER_H
#define AAUDIORECORDER_AAUDIORECORDER_H
#include <fstream>
#include <iosfwd>
#include <thread>
#include <aaudio/AAudio.h>
#include <android/log.h>

#include "modules/audio_processing/include/audio_processing.h"


#include "api/audio/audio_processing_statistics.h"
#include "webRtcApm/AudioRingBuffer.h"


#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "NDKRecorder", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "NDKRecorder", __VA_ARGS__)


class CallbackPCMRecorder {
public:
    CallbackPCMRecorder() : stream(nullptr), builder(nullptr) {}

    bool start(const char* source, const char* filename) {
        rtcFile.open(filename, std::ios::binary);
        sourceFile.open(source, std::ios::binary);
        if (!rtcFile.is_open()) {
            LOGE("Failed to open output file");
            return false;
        }

        if (!sourceFile.is_open()) {
            LOGE("Failed to open sourceFile file");
            return false;
        }

        aaudio_result_t result = AAudio_createStreamBuilder(&builder);
        if (result != AAUDIO_OK) {
            LOGE("Failed to create stream builder");
            return false;
        }

        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
        AAudioStreamBuilder_setSampleRate(builder, SAMPLE_RATE);
        AAudioStreamBuilder_setChannelCount(builder, CHANNELS);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);

        // 设置回调
        AAudioStreamBuilder_setDataCallback(builder, dataCallback, this);
        AAudioStreamBuilder_setErrorCallback(builder, errorCallback, this);

        result = AAudioStreamBuilder_openStream(builder, &stream);
        if (result != AAUDIO_OK) {
            LOGE("Failed to open stream");
            return false;
        }

        result = AAudioStream_requestStart(stream);
        if (result != AAUDIO_OK) {
            LOGE("Failed to start stream");
            return false;
        }

        LOGI("Callback PCM recording started");

        webrtc::AudioProcessing::Config config;


        //高通滤波
        config.high_pass_filter.enabled = true;

        // 配置 AEC、NS、AGC 等
        config.echo_canceller.enabled = true;
        config.noise_suppression.enabled = true;
        config.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kHigh;
        config.gain_controller2.enabled = true;

        webrtc::AudioProcessingBuilder builder;

        builder.SetConfig(config);

        apm = builder.Create();
        //
        // LOGI("webrtc audio processing module create complete");
        // ringBuffer = std::make_unique<AudioRingBuffer>(480 * 20);

        // running = true;
        // processingThread = std::thread(&CallbackPCMRecorder::processAudio, this);
        //
        // LOGI("webrtc audio processing module thread start....");
        // processingThread.detach();


        return true;
    }

    void stop() {
        if (stream) {
            AAudioStream_requestStop(stream);
            AAudioStream_close(stream);
            stream = nullptr;
        }
        if (builder) {
            AAudioStreamBuilder_delete(builder);
            builder = nullptr;
        }
        if (sourceFile.is_open()) sourceFile.close();

        LOGI("Callback PCM recording stopped");
    }

    const int FRAME_SIZE = 480;  // 每次读取 480 帧
    const int SAMPLE_SIZE = 2;   // 每帧 16-bit = 2 字节

    void apmHandlePcm(const char * str) {

        // 缓冲区：每次读取 480 帧 PCM 数据
        std::vector<int16_t> pcm_buffer(FRAME_SIZE);

        // 打开 PCM 文件
        std::ifstream pcm_file(str, std::ios::binary);
        if (!pcm_file.is_open()) {
            return;
        }

        // 读取文件并处理
        while (true) {
            // 从 PCM 文件中读取 480 帧（960 字节）数据
            pcm_file.read(reinterpret_cast<char*>(pcm_buffer.data()), FRAME_SIZE * SAMPLE_SIZE);
            if (pcm_file.gcount() == 0) {
                break;  // 文件结束
            }

            // 确保读取到的数据大小为 480 帧
            if (pcm_file.gcount() != FRAME_SIZE * SAMPLE_SIZE) {
                break;
            }

            // 转换为 float 数据
            std::unique_ptr<float[]> inputChannel(new float[960]);
            std::unique_ptr<float[]> outputChannel(new float[960]);

            for (int i = 0; i < 960; ++i) {
                inputChannel[i] = pcm_buffer[i] / 32767.0f;
            }

            // 创建 WebRTC APM 配置
            webrtc::StreamConfig inputConfig(48000, 1);  // 采样率48000Hz，单通道
            webrtc::StreamConfig outputConfig(48000, 1);

            // 调用 WebRTC APM 进行音频处理
            float* inputPointer = inputChannel.get();
            float* outputPointer = outputChannel.get();

            int result = apm->ProcessStream(
                &inputPointer,  // 输入指针
                inputConfig,
                outputConfig,
                &outputPointer  // 输出指针
            );

            if (result == 0) {

               LOGI("Audio processing success!");
                writeAudioDataToFile(outputPointer, 480);
            } else {
                LOGI("Audio processing failure!");
            }
        }

        if (rtcFile.is_open()) rtcFile.close();
    }

private:
    AAudioStream* stream;
    AAudioStreamBuilder* builder;
    std::ofstream rtcFile;
    std::ofstream sourceFile;

    rtc::scoped_refptr<webrtc::AudioProcessing> apm;

    std::unique_ptr<AudioRingBuffer> ringBuffer;  // 环形缓冲区实例

    std::atomic<bool> running{false};
    std::thread processingThread;
    std::mutex mutex;
    std::condition_variable cv;

    static constexpr int SAMPLE_RATE = 48000;
    static constexpr int CHANNELS = 1;

    void writeAudioDataToFile(float * output_pointer, int32_t numFrames) {
        if (!rtcFile.is_open()) {
            LOGE("Output file is not open.");
            return;
        }

        // 创建一个 int16_t 数组来存储转换后的数据
        std::vector<int16_t> pcmBuffer(numFrames);

        // 将 float 数据转换为 int16_t 格式
        for (int32_t i = 0; i < numFrames; ++i) {
            pcmBuffer[i] = static_cast<int16_t>(std::clamp(output_pointer[i] * 32768.0f, -32768.0f, 32767.0f));
        }

        LOGI("Writing %d frames to file, current file size: %d bytes", numFrames, rtcFile.tellp());

        // 一次性写入文件
        rtcFile.write(reinterpret_cast<char*>(pcmBuffer.data()), numFrames * sizeof(int16_t));

        LOGI("Processed audio data written to file. %d", numFrames);
    }

    static aaudio_data_callback_result_t dataCallback(
    AAudioStream* stream,
    void* userData,
    void* audioData,
    int32_t numFrames
) {
        LOGI("dataCallback invoke audio nums %d", numFrames);
        auto* recorder = static_cast<CallbackPCMRecorder*>(userData);

        recorder->sourceFile.write(static_cast<const char*>(audioData), numFrames * sizeof(int16_t));

        // if (!recorder->ringBuffer->push(audioData, numFrames)) {
        //     LOGE("Failed to push data to buffer.");
        // }

        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    // 错误回调
    static void errorCallback(
        AAudioStream* stream,
        void* userData,
        aaudio_result_t error
    ) {
        LOGE("AAudio error: %d", error);
    }


    // 处理音频数据的线程
    void processAudio() {
        // while (true) {
        //
        //     LOGI("processAudio invoke...");
        //
        //     std::unique_lock<std::mutex> lock(mutex);
        //
        //     cv.wait(lock, [this]() { return !ringBuffer->empty(); });
        //
        //     int16_t inputBuffer[480];
        //     if (!ringBuffer->pop(inputBuffer, 480)) {
        //         LOGI("Audio processing pop failure!");
        //         continue;
        //     }
        //
        //     rtcFile.write(reinterpret_cast<const char*>(inputBuffer), 480 * sizeof(int16_t));
        //
        //     // // 转换为 float 数据
        //     // std::unique_ptr<float[]> inputChannel(new float[480]);
        //     // std::unique_ptr<float[]> outputChannel(new float[480]);
        //     //
        //     // for (int i = 0; i < 480; ++i) {
        //     //     inputChannel[i] = inputBuffer[i] / 32767.0f;
        //     // }
        //     //
        //     // // 创建 WebRTC APM 配置
        //     // webrtc::StreamConfig inputConfig(48000, 1);  // 采样率48000Hz，单通道
        //     // webrtc::StreamConfig outputConfig(48000, 1);
        //     //
        //     // // 调用 WebRTC APM 进行音频处理
        //     // float* inputPointer = inputChannel.get();
        //     // float* outputPointer = outputChannel.get();
        //     //
        //     // int result = apm->ProcessStream(
        //     //     &inputPointer,  // 输入指针
        //     //     inputConfig,
        //     //     outputConfig,
        //     //     &outputPointer  // 输出指针
        //     // );
        //     //
        //     // if (result == 0) {
        //     //
        //     //    LOGI("Audio processing success!, space is %lu", ringBuffer->availableSpace());
        //     //     writeAudioDataToFile(outputPointer, 480);
        //     // } else {
        //     //     LOGI("Audio processing failure!");
        //     // }
        //
        //
        // }

        const size_t FRAMES_PER_PROCESS = 480;
        int16_t processBuffer[FRAMES_PER_PROCESS];

        LOGI("Process thread started.");

        while (running) {
            std::unique_lock<std::mutex> lock(mutex);

            cv.wait(lock, [this]() {
                return ringBuffer->availableData() >= 480 || !running;
            });

            if (!running) {
                LOGI("Stopping process thread, discarding remaining data.");
                break;
            }

            // 精确 pop 480 帧
            if (!ringBuffer->pop(processBuffer, FRAMES_PER_PROCESS)) {
                LOGE("pop failed unexpectedly");
                continue;
            }

            lock.unlock();

            // ================== 这里是你真正的处理逻辑 ==================
            // 例如写入文件（当前示例）
            rtcFile.write(reinterpret_cast<const char*>(processBuffer),
                             FRAMES_PER_PROCESS * sizeof(int16_t));

            // 或者替换成你的编码器、上传、网络发送等：
            // encoder.encode(processBuffer, FRAMES_PER_PROCESS);
            // uploadNetwork(processBuffer, FRAMES_PER_PROCESS);
            // =================================================================
        }

        LOGI("Process thread exited.");
    }

};

#endif //AAUDIORECORDER_AAUDIORECORDER_H
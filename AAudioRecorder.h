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

#include "lwrb.h"


#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "NDKRecorder", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "NDKRecorder", __VA_ARGS__)

//10ms
#define BUFFER_SIZE 960 * 10

class CallbackPCMRecorder {
public:
    CallbackPCMRecorder() : stream(nullptr), builder(nullptr) {
        lwrb_init(&audio_rb, audio_rb_data, BUFFER_SIZE);

    }

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

        LOGI("webrtc audio processing module create complete");

        running = true;
        handlerThread = std::thread(&CallbackPCMRecorder::handlerLoop, this);

        return true;
    }

    void handlerLoop() {
        std::vector<int16_t> pcm(FRAME_SIZE);  // 每次处理 480 帧
        std::unique_ptr<float[]> inputChannel(new float[FRAME_SIZE]);
        std::unique_ptr<float[]> outputChannel(new float[FRAME_SIZE]);

        while (running) {
            std::unique_lock<std::mutex> lock(mutex);

            // 等待环形缓冲区有足够数据
            rb_cv.wait(lock, [&] {
                return !running || lwrb_get_full(&audio_rb) >= FRAME_SIZE * sizeof(int16_t);
            });

            if (!running) break;

            // 从环形缓冲区读取数据
            size_t bytes_to_read = FRAME_SIZE * sizeof(int16_t);
            size_t actually_read = lwrb_read(&audio_rb, (uint8_t*)pcm.data(), bytes_to_read);

            lock.unlock();

            // 写入原始 PCM 文件
            if (sourceFile.is_open()) {
                sourceFile.write(reinterpret_cast<const char*>(pcm.data()), bytes_to_read);
            }

            // 转换为 float 数据
            std::unique_ptr<float[]> inputChannel(new float[480]);
            std::unique_ptr<float[]> outputChannel(new float[480]);

            for (int i = 0; i < 480; ++i) {
                inputChannel[i] = pcm[i] / 32767.0f;
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

                // 转换 float -> int16
                std::vector<int16_t> processedPCM(FRAME_SIZE);
                for (int i = 0; i < FRAME_SIZE; ++i) {
                    processedPCM[i] = static_cast<int16_t>(
                        std::clamp(outputPointer[i] * 32768.0f, -32768.0f, 32767.0f)
                    );
                }

                // 写入文件
                if (rtcFile.is_open()) {
                    rtcFile.write(reinterpret_cast<const char*>(processedPCM.data()), FRAME_SIZE * sizeof(int16_t));
                }
            } else {
                LOGI("Audio processing failure!");
            }
        }
    }

    void stop() {
        running = false;

        // 通知线程退出
        rb_cv.notify_all();

        // 等待线程退出
        if (handlerThread.joinable()) {
            handlerThread.join();
        }

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
        if (rtcFile.is_open()) rtcFile.close();

        LOGI("Callback PCM recording stopped");
    }

    // 析构函数也要确保线程已经退出
    ~CallbackPCMRecorder() {
        stop();
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
                LOGI("Audio processing success!");

                // 转换 float -> int16
                std::vector<int16_t> processedPCM(FRAME_SIZE);
                for (int i = 0; i < FRAME_SIZE; ++i) {
                    processedPCM[i] = static_cast<int16_t>(
                        std::clamp(outputPointer[i] * 32768.0f, -32768.0f, 32767.0f)
                    );
                }

                // 写入文件
                if (rtcFile.is_open()) {
                    rtcFile.write(reinterpret_cast<const char*>(processedPCM.data()), FRAME_SIZE * sizeof(int16_t));
                }
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

    // Ring buffer
    lwrb_t audio_rb;
    uint8_t audio_rb_data[BUFFER_SIZE];

    std::atomic<bool> running{false};
    std::thread handlerThread;

    std::mutex mutex;
    std::condition_variable rb_cv;

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

        // LOGI("Writing %d frames to file, current file size: %d bytes", numFrames, rtcFile.tellp());

        // 一次性写入文件
        rtcFile.write(reinterpret_cast<char*>(pcmBuffer.data()), numFrames * sizeof(int16_t));

        // LOGI("Processed audio data written to file. %d", numFrames);
    }

    static aaudio_data_callback_result_t dataCallback(
    AAudioStream* stream,
    void* userData,
    void* audioData,
    int32_t numFrames
) {
        LOGI("dataCallback invoke audio nums %d", numFrames);
        auto* recorder = static_cast<CallbackPCMRecorder*>(userData);

        auto *in = static_cast<int16_t *>(audioData);
        size_t free_space = lwrb_get_free(&recorder->audio_rb) / sizeof(int16_t);
        size_t to_write = numFrames;

        if (to_write > free_space) {
            to_write = free_space;
            LOGE("Ring buffer overflow, dropping audio data free_space %zu", free_space);
        }

        LOGI("Ring buffer, to write audio data to_write %zu", to_write);

        if (to_write > 0) {
            lwrb_write(&recorder->audio_rb, (uint8_t *) in, to_write * sizeof(int16_t));
            recorder->rb_cv.notify_one();
        }

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
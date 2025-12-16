#ifndef AAUDIORECORDER_AUDIORINGBUFFER_H
#define AAUDIORECORDER_AUDIORINGBUFFER_H

#include <vector>
#include <mutex>
#include <condition_variable>
#include <android/log.h>

#define LOGI_BUFFER(...) __android_log_print(ANDROID_LOG_INFO, "NDKRecorder2", __VA_ARGS__)
#define LOGE_BUFFER(...) __android_log_print(ANDROID_LOG_ERROR, "NDKRecorder2", __VA_ARGS__)

class AudioRingBuffer {
public:
    explicit AudioRingBuffer(size_t capacity)
        : buffer(capacity), head(0), tail(0), full(false) {}

    bool push(const void* data, size_t numFrames) {
        std::unique_lock<std::mutex> lock(mutex);

        size_t space = availableSpace();
        if (numFrames > space) {
            return false;
        }

        const int16_t* input = static_cast<const int16_t*>(data);
        size_t index = tail;

        for (size_t i = 0; i < numFrames; ++i) {
            buffer[index] = input[i];
            index = (index + 1) % buffer.size();
        }

        tail = index;
        full = (tail == head);

        cv.notify_one();

        return true;
    }

    bool pop(void* data, size_t numFrames) {
        std::unique_lock<std::mutex> lock(mutex);

        size_t available = availableData();
        if (numFrames > available) {
            return false;
        }

        int16_t* output = static_cast<int16_t*>(data);
        size_t index = head;

        for (size_t i = 0; i < numFrames; ++i) {
            output[i] = buffer[index];
            index = (index + 1) % buffer.size();
        }

        head = index;
        full = false;

        return true;
    }

    size_t availableSpace() const {
        std::lock_guard<std::mutex> lock(mutex);
        if (full) return 0;
        return buffer.size() - availableData();
    }

    size_t availableData() const {
        std::lock_guard<std::mutex> lock(mutex);
        if (full) return buffer.size();
        if (tail >= head) {
            return tail - head;
        } else {
            return buffer.size() - (head - tail);
        }
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex);
        return (!full && head == tail);
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        head = tail = 0;
        full = false;
    }

private:
    std::vector<int16_t> buffer;
    size_t head = 0;
    size_t tail = 0;
    bool full = false;
    mutable std::mutex mutex;
    std::condition_variable cv;
};

#endif //AAUDIORECORDER_AUDIORINGBUFFER_H
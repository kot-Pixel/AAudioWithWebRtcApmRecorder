#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "AAudioRecorder.h"

namespace {

constexpr int kDefaultDurationSec = 20;
constexpr int kDefaultDelayMs = 100;
constexpr const char* kDefaultFarPath = "/sdcard/far.pcm";

void printUsage(const char* program) {
    LOGI("Usage: %s [duration_sec] [far_pcm] [delay_ms]", program);
    LOGI("  duration_sec : run time in seconds (default: %d)", kDefaultDurationSec);
    LOGI("  far_pcm      : far-end reference PCM, 16k/mono/s16 (default: %s)", kDefaultFarPath);
    LOGI("  delay_ms     : AEC stream delay (default: %d)", kDefaultDelayMs);
    LOGI("Examples:");
    LOGI("  %s", program);
    LOGI("  %s 30", program);
    LOGI("  %s 600 /data/local/tmp/far.pcm 120", program);
}

int parsePositiveInt(const char* value, int minValue, int maxValue) {
    if (value == nullptr || *value == '\0') {
        return -1;
    }

    char* end = nullptr;
    long parsed = std::strtol(value, &end, 10);
    if (*end != '\0' || parsed < minValue || parsed > maxValue) {
        return -1;
    }
    return static_cast<int>(parsed);
}

}  // namespace

int main(int argc, char* argv[]) {
    int durationSec = kDefaultDurationSec;
    int delayMs = kDefaultDelayMs;
    const char* farPath = kDefaultFarPath;

    if (argc >= 2) {
        if (std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "--help") == 0) {
            printUsage(argv[0]);
            return 0;
        }

        int parsedDuration = parsePositiveInt(argv[1], 1, 24 * 60 * 60);
        if (parsedDuration < 0) {
            LOGE("Invalid duration: %s", argv[1]);
            printUsage(argv[0]);
            return 1;
        }
        durationSec = parsedDuration;
    }

    if (argc >= 3) {
        farPath = argv[2];
    }
    if (argc >= 4) {
        int parsedDelay = parsePositiveInt(argv[3], 0, 1000);
        if (parsedDelay < 0) {
            LOGE("Invalid delay_ms: %s", argv[3]);
            printUsage(argv[0]);
            return 1;
        }
        delayMs = parsedDelay;
    }

    LOGI("Config: duration=%d s, delay=%d ms, far=%s", durationSec, delayMs, farPath);

    CallbackPCMRecorder recorder;

    bool startResult = recorder.start(farPath, delayMs);
    LOGI("start aaudio recorder result is: %d", startResult);
    if (!startResult) {
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::seconds(durationSec));
    recorder.stop();
    return 0;
}

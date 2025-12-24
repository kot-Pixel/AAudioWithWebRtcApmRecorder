#include <iostream>
#include "AAudioRecorder.h"

#include <thread>
#include <chrono>

int main() {

    CallbackPCMRecorder recorder;

    bool startResult = recorder.start("/sdcard/source.pcm", "/sdcard/record.pcm");

    LOGI("start aaudio recorder result is: %d", startResult);
    
    std::this_thread::sleep_for(std::chrono::seconds(20));

    recorder.stop();

    return 0;
}
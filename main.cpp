#include <iostream>
#include "AAudioRecorder.h"
#include "df.h"

#include <thread>
#include <chrono>

int main() {

    // CallbackPCMRecorder recorder;
    //
    // bool startResult = recorder.start("/sdcard/source.pcm", "/sdcard/record.pcm");
    //
    // LOGI("start aaudio recorder result is: %d", startResult);
    //
    // std::this_thread::sleep_for(std::chrono::seconds(20));
    //
    // recorder.stop();

    DFState *state = df_create("/sdcard/DeepFilterNet2_onnx_ll.tar.gz", 20, "info");

    if (state != nullptr) {
        LOGI("df_create is not null");
    } else {
        LOGI("df_create is null");
    }

    return 0;
}
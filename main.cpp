#include "AAudioRecorder.h"
#include "df.h"


int main() {

    CallbackPCMRecorder recorder;

    bool startResult = recorder.start("/sdcard/source.pcm", "/sdcard/record.pcm", "/sdcard/deep.pcm");

    LOGI("start aaudio recorder result is: %d", startResult);

    std::this_thread::sleep_for(std::chrono::seconds(20));

    recorder.stop();

    return 0;
}
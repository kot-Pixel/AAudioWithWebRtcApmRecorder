cmake_minimum_required(VERSION 3.22.1)

project(AAudioRecorder)

set(CMAKE_CXX_STANDARD 17)


set(WEBRTC_APM_ROOT ${CMAKE_CURRENT_LIST_DIR}/webRtcApm)
set(WEBRTC_LIB_DIR ${WEBRTC_APM_ROOT}/lib)

set(WEBRTC_INCLUDE_DIR
        ${WEBRTC_APM_ROOT}/include
)


add_library(webrtc_apm SHARED IMPORTED)

set_target_properties(webrtc_apm PROPERTIES
        IMPORTED_LOCATION ${WEBRTC_LIB_DIR}/libwebrtc_audio_processing.a
)

add_executable(AAudioRecorder main.cpp
        AAudioRecorder.cpp
        AAudioRecorder.h
        webRtcApm/AudioRingBuffer.cpp
        webRtcApm/AudioRingBuffer.h)

target_include_directories(AAudioRecorder PRIVATE
        ${WEBRTC_INCLUDE_DIR}
        ${WEBRTC_INCLUDE_DIR}/webrtc-audio-processing-2
)

target_link_libraries(
        AAudioRecorder PRIVATE
        aaudio

        webrtc_apm
        log
)

install(FILES AAudioRecorder.h
        DESTINATION include
)

install(TARGETS AAudioRecorder
        RUNTIME DESTINATION bin
)
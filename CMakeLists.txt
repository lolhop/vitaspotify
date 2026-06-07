cmake_minimum_required(VERSION 3.10)
# cspot/bell still declare CMake 2.8; required for CMake 4.x hosts
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(VITA_APP_NAME "VitaSpotify")
set(VITA_TITLEID "VSPOT0001")
set(VITA_VERSION "01.03")

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VITASDK})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
    else()
        message(FATAL_ERROR "Set VITASDK to your VitaSDK install (see README.md).")
    endif()
endif()

project(vitaspotify C CXX)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu11")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -std=c++20")
set(CMAKE_CXX_STANDARD 20)

# Vita-specific queue (mutex/cond) for bell
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/patch/Queue.h
     DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cspot/cspot/bell/main/utilities/include)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/patch/TCPSocket.h
     DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cspot/cspot/bell/main/io/include)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/patch/WrappedSemaphore.h
     DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cspot/cspot/bell/main/platform)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/patch/HTTPClient.h
     DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cspot/cspot/bell/main/io/include)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/patch/HTTPClient.cpp
     DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cspot/cspot/bell/main/io)

set(BELL_DISABLE_WEBSERVER ON CACHE BOOL "" FORCE)
set(BELL_DISABLE_MQTT ON CACHE BOOL "" FORCE)

# Cross-compiling on macOS: use pthread platform code, not host dispatch.
set(APPLE OFF CACHE BOOL "" FORCE)

add_compile_definitions(VITA)
add_compile_definitions(BELL_USE_MBEDTLS)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/third_party/cspot/cspot
                 ${CMAKE_CURRENT_BINARY_DIR}/cspot)

target_sources(bell PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/patch/WrappedSemaphore_vita.cpp
)

find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)

include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/debugscreen
    ${CMAKE_SOURCE_DIR}/third_party/cspot/cspot/include
    ${CMAKE_SOURCE_DIR}/third_party/cspot/cspot/bell/main/utilities/include
    ${CMAKE_SOURCE_DIR}/third_party/cspot/cspot/bell/main/io/include
    ${CMAKE_SOURCE_DIR}/third_party/cspot/cspot/bell/main/audio-dsp/include
    ${CMAKE_SOURCE_DIR}/third_party/cspot/cspot/bell/external/cJSON
)

set(DEBUG_SCREEN_SOURCES
    third_party/debugscreen/debugScreen.c
)
set_source_files_properties(${DEBUG_SCREEN_SOURCES} PROPERTIES LANGUAGE C)

add_executable(${PROJECT_NAME}
    src/main.cpp
    src/App.cpp
    src/API.cpp
    src/Gfx.cpp
    src/Touch.cpp
    src/Utils.cpp
    src/Log.cpp
    src/Keyboard.cpp
    src/CliFile.cpp
    src/VitaAudioSink.cpp
    src/VitaPlayer.cpp
    src/VitaRingBuffer.cpp
    src/OfflineStore.cpp
    src/LocalPlayer.cpp
    ${DEBUG_SCREEN_SOURCES}
)

target_compile_definitions(${PROJECT_NAME} PRIVATE VITA_APP_VERSION="${VITA_VERSION}")

target_link_libraries(${PROJECT_NAME}
    vita2d
    SceBgAppUtil_stub
    SceNotificationUtil_stub
    pthread
    cspot
    m
    SceAudio_stub
    SceShellSvc_stub
    SceAppMgr_stub
    SceAppUtil_stub
    SceCommonDialog_stub
    SceIme_stub
    SceCtrl_stub
    SceDisplay_stub
    SceGxm_stub
    ScePgf_stub
    SceLibKernel_stub
    ScePower_stub
    SceSysmodule_stub
    SceTouch_stub
    taihen_stub
    freetype
    png
    jpeg
    ${CURL_LIBRARIES}
    ${OPENSSL_LIBRARIES}
    z
)

vita_create_self(eboot.bin ${PROJECT_NAME} UNSAFE
    ATTRIBUTE 0x0000000E
    MEMSIZE 0x0
)

# Standard homebrew app metadata (avoid "game" category — easier LiveArea uninstall)
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE=17338440 -d ATTRIBUTE2=0 -d ATTRIBUTE_MINOR=17")

vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} eboot.bin
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
    FILE assets/sce_sys/icon0.png sce_sys/icon0.png
    FILE assets/ui/back.png assets/ui/back.png
    FILE assets/ui/forwards.png assets/ui/forwards.png
    FILE assets/ui/pauseplay.png assets/ui/pauseplay.png
    FILE assets/ui/repeat.png assets/ui/repeat.png
    FILE assets/ui/shuffle.png assets/ui/shuffle.png
)

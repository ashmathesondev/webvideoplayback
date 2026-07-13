cmake_minimum_required(VERSION 3.25)

project(webvideoplayback VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(SDL3 CONFIG REQUIRED)
find_package(FFMPEG REQUIRED)
find_package(ftxui CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(webvideoplayback WIN32
    src/main.cpp
    src/common/string_utils.cpp
    src/player/audio_output.cpp
    src/player/av_support.cpp
    src/player/decoder_backend.cpp
    src/player/ffmpeg_media_decoder.cpp
    src/player/media_foundation_decoder.cpp
    src/player/player.cpp
    src/player/render_sink.cpp
    src/player/sdl_platform.cpp
)

target_include_directories(webvideoplayback PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${FFMPEG_INCLUDE_DIRS}
)

target_link_directories(webvideoplayback PRIVATE
    ${FFMPEG_LIBRARY_DIRS}
)

target_compile_definitions(webvideoplayback PRIVATE
    WEBVIDEOPLAYBACK_REPORT_DIR="${CMAKE_SOURCE_DIR}/reports"
)

target_link_libraries(webvideoplayback PRIVATE
    SDL3::SDL3
    ${FFMPEG_LIBRARIES}
)

if(WIN32)
    target_link_libraries(webvideoplayback PRIVATE comdlg32 mfplat mfreadwrite mfuuid ole32 psapi)

    add_executable(webvideoplayback_test_server
        src/test_video_server.cpp
        src/common/string_utils.cpp
        src/server/network.cpp
        src/server/server.cpp
    )

    target_include_directories(webvideoplayback_test_server PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )

    target_link_libraries(webvideoplayback_test_server PRIVATE ws2_32)
    target_link_libraries(webvideoplayback_test_server PRIVATE
        ftxui::screen
        ftxui::dom
        ftxui::component
        nlohmann_json::nlohmann_json
    )
endif()

if(MSVC)
    target_compile_options(webvideoplayback PRIVATE /W4 /permissive-)
else()
    target_compile_options(webvideoplayback PRIVATE -Wall -Wextra -Wpedantic)
endif()

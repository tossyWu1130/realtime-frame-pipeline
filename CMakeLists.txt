cmake_minimum_required(VERSION 3.16)
project(realtime_frame_pipeline LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Threads REQUIRED)
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

add_executable(realtime_frame_pipeline
    src/main.cpp
    src/main_window.cpp
    src/frame_pipeline.cpp
    src/precision_timer.cpp
)

target_include_directories(realtime_frame_pipeline PRIVATE src)

target_link_libraries(realtime_frame_pipeline PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Threads::Threads
)

# timeBeginPeriod / timeEndPeriod live in winmm.
if(WIN32)
    target_link_libraries(realtime_frame_pipeline PRIVATE winmm)
endif()

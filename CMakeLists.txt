cmake_minimum_required(VERSION 3.21)

project(mp4player C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Your repo already contains an FFmpeg "dev" folder at ./ffmpeg/
set(FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/ffmpeg" CACHE PATH "Path to FFmpeg root (must contain include/ and lib/)")

add_executable(mp4player
  main.c
)

target_include_directories(mp4player PRIVATE
  "${FFMPEG_ROOT}/include"
)

find_library(AVFORMAT_LIB avformat HINTS "${FFMPEG_ROOT}/lib" REQUIRED)
find_library(AVCODEC_LIB  avcodec  HINTS "${FFMPEG_ROOT}/lib" REQUIRED)
find_library(AVUTIL_LIB   avutil   HINTS "${FFMPEG_ROOT}/lib" REQUIRED)
find_library(SWSCALE_LIB  swscale  HINTS "${FFMPEG_ROOT}/lib" REQUIRED)
find_library(SWRESAMPLE_LIB swresample HINTS "${FFMPEG_ROOT}/lib" REQUIRED)

target_link_libraries(mp4player PRIVATE
  ${AVFORMAT_LIB}
  ${AVCODEC_LIB}
  ${AVUTIL_LIB}
  ${SWSCALE_LIB}
  ${SWRESAMPLE_LIB}
  winmm
  user32
  gdi32
)

# Copy FFmpeg runtime DLLs next to the built .exe so it runs.
if(WIN32)
  add_custom_command(TARGET mp4player POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${FFMPEG_ROOT}/bin"
      "$<TARGET_FILE_DIR:mp4player>"
    VERBATIM
  )
endif()


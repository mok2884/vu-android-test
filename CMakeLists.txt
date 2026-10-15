cmake_minimum_required(VERSION 3.18.1)
project(vuaaudio C)
add_library(vuaaudio SHARED aaudio_jni.c)
target_link_libraries(vuaaudio aaudio dl)

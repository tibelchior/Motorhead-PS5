cmake_minimum_required(VERSION 3.28)
project(Motorhead VERSION 0.1.0 LANGUAGES C CXX)
# The PS5 target is the Linux platform layer with the software renderer only (no OpenGL, no X11).
set(MH_LINUX_VARIANT ON)
set(MH_PS5_VARIANT ON)
include(../shared/cmake/Motorhead.cmake)

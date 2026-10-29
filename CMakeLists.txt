cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake"
        CACHE PATH "VitaSDK toolchain file")
endif()

if(NOT DEFINED ENV{VITASDK})
    message(FATAL_ERROR "VITASDK is not set. Install VitaSDK or build in the official VitaSDK container.")
endif()

include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

project(Vlender C)

set(VITA_APP_NAME "Vlender")
set(VITA_TITLEID "VLD000001")
set(VITA_VERSION "01.00")

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(Vlender
    src/main.c
)

target_compile_options(Vlender PRIVATE
    -Wall
    -Wextra
    -Wshadow
    -Wconversion
    -Wno-sign-conversion
)

target_link_libraries(Vlender PRIVATE
    vita2d
    SceDisplay_stub
    SceCtrl_stub
    SceTouch_stub
    SceGxm_stub
    SceSysmodule_stub
    SceIofilemgr_stub
    SceAppMgr_stub
    SceProcessmgr_stub
    ScePgf_stub
    ScePvf_stub
    SceCommonDialog_stub
    freetype
    png
    jpeg
    z
    m
    c
)

vita_create_self(eboot.bin Vlender UNSAFE)

vita_create_vpk(Vlender.vpk ${VITA_TITLEID} eboot.bin
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
)

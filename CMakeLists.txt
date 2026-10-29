cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake"
        CACHE PATH "VitaSDK toolchain file")
endif()

if(NOT DEFINED ENV{VITASDK})
    message(FATAL_ERROR "VITASDK is not set. Install VitaSDK or use the official VitaSDK container.")
endif()

include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
project(Vlender C)

set(VITA_APP_NAME "Vlender" CACHE STRING "Name shown in the PlayStation Vita LiveArea")
set(VITA_TITLEID "VLD000001")
set(VITA_VERSION "01.00")

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(Vlender
    src/main.c
    src/model.c
)

target_compile_options(Vlender PRIVATE
    -Os
    -fno-lto
    -ffunction-sections
    -fdata-sections
    -fno-unwind-tables
    -fno-asynchronous-unwind-tables
    -Wall
    -Wextra
    -Wno-sign-conversion
)

# vitaGL provides hardware-accelerated 3D. Drop unreachable code and unwind
# metadata to leave the Vita SELF packager its required space at the end of
# the executable segment as the editor grows.
target_link_options(Vlender PRIVATE -Wl,-q -Wl,--gc-sections -fno-lto)
target_link_libraries(Vlender PRIVATE
    vitaGL
    stdc++
    c
    SceCommonDialog_stub
    m
    SceGxm_stub
    SceDisplay_stub
    SceAppMgr_stub
    SceCtrl_stub
    SceTouch_stub
    SceSysmodule_stub
    SceIofilemgr_stub
    SceProcessmgr_stub
    mathneon
    vitashark
    SceShaccCgExt
    taihen_stub
    SceShaccCg_stub
    SceKernelDmacMgr_stub
)

vita_create_self(eboot.bin Vlender UNSAFE)

vita_create_vpk(Vlender.vpk ${VITA_TITLEID} eboot.bin
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
    FILE assets/branding/icon0.png sce_sys/icon0.png
    FILE assets/branding/livearea/bg.png sce_sys/livearea/contents/bg.png
    FILE assets/branding/livearea/startup.png sce_sys/livearea/contents/startup.png
    FILE assets/branding/livearea/template.xml sce_sys/livearea/contents/template.xml
)

# CMakeLists.txt for OMT Bridge in Open Camera
# This compiles the JNI bridge that connects Java to our C++ OMT libraries

cmake_minimum_required(VERSION 3.22.1)
project("opencamera_omt")

# =============================================================================
# OMT (Open Media Transport) Libraries
# =============================================================================

# Build libomt from source (our custom C++ implementation with quality support)
# Same structure as omtandroid project
set(LIBOMT_DIR ${CMAKE_SOURCE_DIR}/../../../../libomt)
set(LIBVMX_DIR ${CMAKE_SOURCE_DIR}/../../../../libvmx)

# Build VMX codec from source with ARM64 NEON optimizations
# CRITICAL: Must compile vmxcodec.cpp and vmxcodec_arm.cpp with -DARM64
add_library(vmx SHARED
    ${LIBVMX_DIR}/src/vmxcodec.cpp
    ${LIBVMX_DIR}/src/vmxcodec_arm.cpp
)

target_include_directories(vmx PRIVATE
    ${LIBVMX_DIR}/src
)

# CRITICAL FLAGS for VMX:
# -DARM64: Enables NEON SIMD optimizations (without this = black frames!)
# -DANDROID: Android-specific code paths
# -fms-extensions -fdeclspec: Required for __declspec(align) in vmxcodec.h
target_compile_options(vmx PRIVATE 
    -O3 
    -fPIC 
    -DARM64 
    -DANDROID 
    -fms-extensions 
    -fdeclspec 
    -Wno-macro-redefined
)

target_link_libraries(vmx
    log
)

# Build libomt from source (modular C++ implementation)
# Ported from libomtnet with proper separation of concerns
add_library(omt SHARED
    ${LIBOMT_DIR}/cpp/libomt_android.cpp
    ${LIBOMT_DIR}/cpp/OMTAsyncPool.cpp
    ${LIBOMT_DIR}/cpp/OMTChannel.cpp
    ${LIBOMT_DIR}/cpp/OMTSender.cpp
    ${LIBOMT_DIR}/cpp/OMTDiscovery.cpp
)

# Include directories for libomt compilation
target_include_directories(omt PRIVATE 
    ${LIBOMT_DIR}/cpp
    ${LIBOMT_DIR}
    ${LIBVMX_DIR}/src
)

# Compiler flags for libomt
target_compile_options(omt PRIVATE 
    -O3
    -fPIC
    -DARM64 
    -DANDROID 
    -fms-extensions 
    -fdeclspec
    -Wno-macro-redefined
    -std=c++17
)

# Link libomt against vmx and Android libs
target_link_libraries(omt
    vmx
    log
)

# =============================================================================
# OMT Bridge (JNI interface for Open Camera)
# =============================================================================

add_library(omtbridge SHARED
    OMTBridge.cpp
)

# Include directories for OMTBridge
target_include_directories(omtbridge PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${LIBOMT_DIR}
)

# Find the log library
find_library(log-lib log)

# Link all libraries together
target_link_libraries(omtbridge
    ${log-lib}
    omt
    vmx
)

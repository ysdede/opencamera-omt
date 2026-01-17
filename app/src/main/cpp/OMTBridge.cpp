/**
 * OMTBridge.cpp - JNI Bridge for Open Media Transport (Open Camera Edition)
 * 
 * This file provides the native interface between Java and the OMT C library.
 * It handles video frame sending from the Android camera to OMT receivers on the network.
 * 
 * Package: net.sourceforge.opencamera
 */

#include <jni.h>
#include <android/log.h>
#include <string>
#include <mutex>

#include "libomt.h"

#define LOG_TAG "OMTBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#ifndef NDEBUG
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#define LOGD(...) ((void)0)
#endif

// Global sender instance (thread-safe access)
static omt_send_t* g_sender = nullptr;
static std::mutex g_senderMutex;

// Frame configuration (set during init, used during send)
static int g_width = 0;
static int g_height = 0;
static int g_frameRateN = 30;
static int g_frameRateD = 1;

extern "C" {

/**
 * Initialize the OMT sender with the given name and video configuration.
 */
JNIEXPORT jboolean JNICALL
Java_net_sourceforge_opencamera_OMTSender_nativeInit(
        JNIEnv* env,
        jobject /* this */,
        jstring name,
        jint width,
        jint height,
        jint frameRateN,
        jint frameRateD,
        jint quality) {
    
    std::lock_guard<std::mutex> lock(g_senderMutex);
    
    // Cleanup existing sender if any
    if (g_sender != nullptr) {
        LOGI("Destroying existing sender before creating new one");
        omt_send_destroy(g_sender);
        g_sender = nullptr;
    }
    
    // Get the sender name from Java string
    const char* senderName = env->GetStringUTFChars(name, nullptr);
    if (senderName == nullptr) {
        LOGE("Failed to get sender name string");
        return JNI_FALSE;
    }
    
    LOGI("Creating OMT sender: %s (%dx%d @ %d/%d fps, Quality: %d)", 
         senderName, width, height, frameRateN, frameRateD, quality);
    
    // Create the OMT sender with specified quality
    g_sender = omt_send_create(senderName, (OMTQuality)quality);
    
    env->ReleaseStringUTFChars(name, senderName);
    
    if (g_sender == nullptr) {
        LOGE("Failed to create OMT sender");
        return JNI_FALSE;
    }
    
    // Store configuration for frame sending
    g_width = width;
    g_height = height;
    g_frameRateN = frameRateN;
    g_frameRateD = frameRateD;
    
    LOGI("OMT sender created successfully");
    return JNI_TRUE;
}

/**
 * Send a video frame to connected receivers.
 */
JNIEXPORT jboolean JNICALL
Java_net_sourceforge_opencamera_OMTSender_nativeSendFrame(
        JNIEnv* env,
        jobject /* this */,
        jobject buffer,
        jint width,
        jint height,
        jint yStride,
        jint uvStride) {
    
    std::lock_guard<std::mutex> lock(g_senderMutex);
    
    if (g_sender == nullptr) {
        LOGE("Cannot send frame: sender not initialized");
        return JNI_FALSE;
    }
    
    // Get the native buffer address
    void* data = env->GetDirectBufferAddress(buffer);
    if (data == nullptr) {
        LOGE("Failed to get direct buffer address");
        return JNI_FALSE;
    }
    
    // Calculate data length for NV12 format
    int dataLength = (height * yStride) + ((height / 2) * uvStride);
    
    // Prepare the media frame
    OMTMediaFrame frame = {};
    frame.Type = OMTFrameType_Video;
    frame.Codec = OMTCodec_NV12;
    frame.Width = width;
    frame.Height = height;
    frame.Stride = yStride;
    frame.Flags = OMTVideoFlags_None;
    frame.FrameRateN = g_frameRateN;
    frame.FrameRateD = g_frameRateD;
    frame.AspectRatio = (float)width / (float)height;
    frame.ColorSpace = (height >= 720) ? OMTColorSpace_BT709 : OMTColorSpace_BT601;
    frame.Timestamp = -1;  // Auto-generate timestamp
    frame.Data = data;
    frame.DataLength = dataLength;
    
    // Send the frame
    int result = omt_send(g_sender, &frame);
    
    if (result != 0) {
        LOGD("Frame sent (result: %d)", result);
    }
    
    return JNI_TRUE;
}

/**
 * Get the number of currently connected receivers.
 */
JNIEXPORT jint JNICALL
Java_net_sourceforge_opencamera_OMTSender_nativeGetConnectionCount(
        JNIEnv* env,
        jobject /* this */) {
    
    std::lock_guard<std::mutex> lock(g_senderMutex);
    
    if (g_sender == nullptr) {
        return 0;
    }
    
    return omt_send_connections(g_sender);
}

/**
 * Get the discovery address string for this sender.
 */
JNIEXPORT jstring JNICALL
Java_net_sourceforge_opencamera_OMTSender_nativeGetAddress(
        JNIEnv* env,
        jobject /* this */) {
    
    std::lock_guard<std::mutex> lock(g_senderMutex);
    
    if (g_sender == nullptr) {
        return nullptr;
    }
    
    char address[OMT_MAX_STRING_LENGTH];
    int length = omt_send_getaddress(g_sender, address, OMT_MAX_STRING_LENGTH);
    
    if (length <= 0) {
        return nullptr;
    }
    
    return env->NewStringUTF(address);
}

/**
 * Cleanup and destroy the OMT sender.
 */
JNIEXPORT void JNICALL
Java_net_sourceforge_opencamera_OMTSender_nativeCleanup(
        JNIEnv* env,
        jobject /* this */) {
    
    std::lock_guard<std::mutex> lock(g_senderMutex);
    
    if (g_sender != nullptr) {
        LOGI("Destroying OMT sender");
        omt_send_destroy(g_sender);
        g_sender = nullptr;
    }
    
    g_width = 0;
    g_height = 0;
}

// =============================================================
// Statistics API (for UI feedback on frame drops)
// =============================================================

// External C functions from libomt
extern int64_t omt_send_get_frames_sent(omt_send_t* instance);
extern int64_t omt_send_get_frames_dropped(omt_send_t* instance);
extern int64_t omt_send_get_recent_drops_and_reset(omt_send_t* instance);
extern int64_t omt_send_get_bytes_sent(omt_send_t* instance);
// Custom helper
extern void omt_send_set_info_str(omt_send_t* instance, const char* product, const char* manufacturer);
extern void omt_send_set_quality(omt_send_t* instance, OMTQuality quality);

/**
 * Set the current VMX quality level.
 */
JNIEXPORT void JNICALL
Java_net_sourceforge_opencamera_OMTSender_nativeSetQuality(
        JNIEnv* env,
        jobject /* this */,
        jint quality) {
    
    std::lock_guard<std::mutex> lock(g_senderMutex);
    if (g_sender == nullptr) return;
    
    LOGI("JNI: Setting OMT quality to %d", quality);
    omt_send_set_quality(g_sender, (OMTQuality)quality);
}

/**
 * Set sender information (product, manufacturer).
 */
JNIEXPORT void JNICALL
Java_net_sourceforge_opencamera_OMTSender_nativeSetSenderInfo(
        JNIEnv* env,
        jobject /* this */,
        jstring productName,
        jstring manufacturer) {
    
    std::lock_guard<std::mutex> lock(g_senderMutex);
    
    if (g_sender == nullptr) return;
    
    const char* productStr = env->GetStringUTFChars(productName, nullptr);
    const char* mfgStr = env->GetStringUTFChars(manufacturer, nullptr);
    
    if (productStr && mfgStr) {
        omt_send_set_info_str(g_sender, productStr, mfgStr);
    }
    
    if (productStr) env->ReleaseStringUTFChars(productName, productStr);
    if (mfgStr) env->ReleaseStringUTFChars(manufacturer, mfgStr);
}

/**
 * Get total frames sent since init.
 */
JNIEXPORT jlong JNICALL
Java_net_sourceforge_opencamera_OMTSender_nativeGetFramesSent(
        JNIEnv* env,
        jobject /* this */) {
    
    std::lock_guard<std::mutex> lock(g_senderMutex);
    if (g_sender == nullptr) return 0;
    return (jlong)omt_send_get_frames_sent(g_sender);
}

/**
 * Get total frames dropped since init.
 */
JNIEXPORT jlong JNICALL
Java_net_sourceforge_opencamera_OMTSender_nativeGetFramesDropped(
        JNIEnv* env,
        jobject /* this */) {
    
    std::lock_guard<std::mutex> lock(g_senderMutex);
    if (g_sender == nullptr) return 0;
    return (jlong)omt_send_get_frames_dropped(g_sender);
}

/**
 * Get recent drops and reset counter (for periodic UI updates).
 * Returns the number of frames dropped since last call.
 */
JNIEXPORT jlong JNICALL
Java_net_sourceforge_opencamera_OMTSender_nativeGetRecentDropsAndReset(
        JNIEnv* env,
        jobject /* this */) {
    
    std::lock_guard<std::mutex> lock(g_senderMutex);
    if (g_sender == nullptr) return 0;
    return (jlong)omt_send_get_recent_drops_and_reset(g_sender);
}

/**
 * Get total bytes sent since init.
 */
JNIEXPORT jlong JNICALL
Java_net_sourceforge_opencamera_OMTSender_nativeGetBytesSent(
        JNIEnv* env,
        jobject /* this */) {
    
    std::lock_guard<std::mutex> lock(g_senderMutex);
    if (g_sender == nullptr) return 0;
    return (jlong)omt_send_get_bytes_sent(g_sender);
}

extern int omt_send_get_profile(omt_send_t* instance);

/**
 * Get current VMX profile ID.
 */
JNIEXPORT jint JNICALL
Java_net_sourceforge_opencamera_OMTSender_nativeGetProfile(
        JNIEnv* env,
        jobject /* this */) {
    
    std::lock_guard<std::mutex> lock(g_senderMutex);
    if (g_sender == nullptr) return 0;
    return omt_send_get_profile(g_sender);
}

} // extern "C"

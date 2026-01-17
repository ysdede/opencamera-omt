/**
 * libomt_android.cpp - C API Wrapper for OMT Android
 * 
 * This file provides the C API defined in libomt.h
 * by wrapping the C++ omt::Sender and omt::Discovery classes.
 */

#include "../libomt.h"
#include "OMTSender.h"
#include "OMTDiscovery.h"
#include "OMTConstants.h"

#include <cstring>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "libomt-api"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...)
#define LOGE(...)
#endif

// Static storage for discovery addresses (returned to caller)
static std::vector<char*> s_discoveryAddresses;

extern "C" {

// =============================================================
// Discovery API
// =============================================================

char** omt_discovery_getaddresses(int* count) {
    // Free previous results
    for (char* addr : s_discoveryAddresses) {
        free(addr);
    }
    s_discoveryAddresses.clear();
    
    // Get addresses from Discovery
    auto addresses = omt::Discovery::getInstance().getAddresses();
    
    if (addresses.empty()) {
        if (count) *count = 0;
        return nullptr;
    }
    
    // Allocate and copy
    s_discoveryAddresses.reserve(addresses.size());
    for (const auto& addr : addresses) {
        char* copy = static_cast<char*>(malloc(addr.size() + 1));
        strcpy(copy, addr.c_str());
        s_discoveryAddresses.push_back(copy);
    }
    
    if (count) *count = static_cast<int>(s_discoveryAddresses.size());
    return s_discoveryAddresses.data();
}

// Additional Discovery API functions

int omt_discovery_start() {
    return omt::Discovery::getInstance().start() ? 1 : 0;
}

void omt_discovery_stop() {
    omt::Discovery::getInstance().stop();
}

int omt_discovery_register(const char* name, int port) {
    if (!name) return 0;
    return omt::Discovery::getInstance().registerService(name, port) ? 1 : 0;
}

int omt_discovery_unregister(const char* name) {
    if (!name) return 0;
    return omt::Discovery::getInstance().unregisterService(name) ? 1 : 0;
}

// JNI callbacks for Android NsdManager
void omt_discovery_on_service_found(const char* name, const char* host, int port) {
    if (!name) return;
    omt::Discovery::getInstance().onServiceDiscovered(
        name, host ? host : "", port);
}

void omt_discovery_on_service_lost(const char* name) {
    if (!name) return;
    omt::Discovery::getInstance().onServiceLost(name);
}

void omt_discovery_on_registered(const char* name) {
    if (!name) return;
    omt::Discovery::getInstance().onRegistered(name);
}

void omt_discovery_on_registration_failed(const char* name, int errorCode) {
    if (!name) return;
    omt::Discovery::getInstance().onRegistrationFailed(name, errorCode);
}

// =============================================================
// Receive API (stubs - not implemented for Android sender)
// =============================================================

omt_receive_t* omt_receive_create(const char* address, OMTFrameType frameTypes, 
                                   OMTPreferredVideoFormat format, OMTReceiveFlags flags) {
    return nullptr;
}

void omt_receive_destroy(omt_receive_t* instance) {}
OMTMediaFrame* omt_receive(omt_receive_t* instance, OMTFrameType frameTypes, int timeoutMilliseconds) { return nullptr; }
int omt_receive_send(omt_receive_t* instance, OMTMediaFrame* frame) { return 0; }
void omt_receive_settally(omt_receive_t* instance, OMTTally* tally) {}
int omt_receive_gettally(omt_send_t* instance, int timeoutMilliseconds, OMTTally* tally) { return 0; }
void omt_receive_setflags(omt_receive_t* instance, OMTReceiveFlags flags) {}
void omt_receive_setsuggestedquality(omt_receive_t* instance, OMTQuality quality) {}
void omt_receive_getsenderinformation(omt_receive_t* instance, OMTSenderInfo* info) {}
void omt_receive_getvideostatistics(omt_receive_t* instance, OMTStatistics* stats) {}
void omt_receive_getaudiostatistics(omt_receive_t* instance, OMTStatistics* stats) {}

// =============================================================
// Send API - Main Implementation
// =============================================================

omt_send_t* omt_send_create(const char* name, OMTQuality quality) {
    LOGI("omt_send_create: name=%s, quality=%d", name ? name : "unnamed", static_cast<int>(quality));
    
    omt::SenderConfig config;
    config.name = name ? name : "OMT Android";
    config.port = omt::Constants::NETWORK_PORT_START;
    
    switch (quality) {
        case OMTQuality_Low: config.quality = omt::Quality::Low; break;
        case OMTQuality_Medium: config.quality = omt::Quality::Medium; break;
        case OMTQuality_High: config.quality = omt::Quality::High; break;
        default: config.quality = omt::Quality::Default; break;
    }
    
    auto* sender = new omt::Sender(config);
    if (!sender->start()) {
        LOGE("omt_send_create: failed to start sender");
        delete sender;
        return nullptr;
    }
    
    return reinterpret_cast<omt_send_t*>(sender);
}

void omt_send_destroy(omt_send_t* instance) {
    if (instance) {
        auto* sender = reinterpret_cast<omt::Sender*>(instance);
        delete sender;
    }
}

int omt_send(omt_send_t* instance, OMTMediaFrame* frame) {
    if (!instance || !frame) return 0;
    
    auto* sender = reinterpret_cast<omt::Sender*>(instance);
    
    // Only handle video for now
    if (frame->Type != OMTFrameType_Video) return 0;
    
    omt::MediaFrame mf{};
    mf.data = frame->Data;
    mf.dataLength = frame->DataLength;
    mf.width = frame->Width;
    mf.height = frame->Height;
    mf.stride = frame->Stride;
    mf.codec = frame->Codec;
    mf.frameRateN = frame->FrameRateN;
    mf.frameRateD = frame->FrameRateD;
    mf.aspectRatio = frame->AspectRatio;
    mf.flags = frame->Flags;
    mf.colorSpace = frame->ColorSpace;
    mf.timestamp = frame->Timestamp;
    
    return sender->send(mf);
}

int omt_send_connections(omt_send_t* instance) {
    if (!instance) return 0;
    auto* sender = reinterpret_cast<omt::Sender*>(instance);
    return sender->connectionCount();
}

// Custom helper for Android Bridge
void omt_send_set_info_str(omt_send_t* instance, const char* product, const char* manufacturer) {
    if (instance && product && manufacturer) {
        auto* sender = reinterpret_cast<omt::Sender*>(instance);
        sender->setSenderInfo(product, manufacturer);
    }
}

void omt_send_setsenderinformation(omt_send_t* instance, OMTSenderInfo* info) {
    // Legacy struct based implementation - skipped for now
}

void omt_send_addconnectionmetadata(omt_send_t* instance, const char* metadata) {
    if (instance && metadata) {
        auto* sender = reinterpret_cast<omt::Sender*>(instance);
        sender->sendMetadata(metadata);
    }
}

void omt_send_clearconnectionmetadata(omt_send_t* instance) {
    // TODO: implement if needed
}

void omt_send_setredirect(omt_send_t* instance, const char* newAddress) {
    // TODO: implement if needed
}

int omt_send_getaddress(omt_send_t* instance, char* address, int maxLength) {
    return 0;
}

OMTMediaFrame* omt_send_receive(omt_send_t* instance, int timeoutMilliseconds) {
    return nullptr;
}

int omt_send_gettally(omt_send_t* instance, int timeoutMilliseconds, OMTTally* tally) {
    if (!instance || !tally) return 0;
    
    auto* sender = reinterpret_cast<omt::Sender*>(instance);
    tally->preview = sender->hasTallyPreview() ? 1 : 0;
    tally->program = sender->hasTallyProgram() ? 1 : 0;
    
    return (tally->preview || tally->program) ? 1 : 0;
}

void omt_send_getvideostatistics(omt_send_t* instance, OMTStatistics* stats) {
    if (!instance || !stats) return;
    
    auto* sender = reinterpret_cast<omt::Sender*>(instance);
    auto s = sender->getStats();
    
    stats->Frames = s.totalFramesSent;
    stats->FramesDropped = s.totalFramesDropped;
    stats->BytesSent = s.totalBytesSent;
}

void omt_send_getaudiostatistics(omt_send_t* instance, OMTStatistics* stats) {
    // Audio not implemented yet
}

// =============================================================
// Settings API (stubs)
// =============================================================

void omt_setloggingfilename(const char* filename) {}
int omt_settings_get_string(const char* name, char* value, int maxLength) { return 0; }
void omt_settings_set_string(const char* name, const char* value) {}
int omt_settings_get_integer(const char* name) { return 0; }
void omt_settings_set_integer(const char* name, int value) {}

// =============================================================
// Extended Statistics API
// =============================================================

int64_t omt_send_get_frames_sent(omt_send_t* instance) {
    if (!instance) return 0;
    auto* sender = reinterpret_cast<omt::Sender*>(instance);
    return sender->getStats().totalFramesSent;
}

int64_t omt_send_get_frames_dropped(omt_send_t* instance) {
    if (!instance) return 0;
    auto* sender = reinterpret_cast<omt::Sender*>(instance);
    return sender->getStats().totalFramesDropped;
}

int64_t omt_send_get_bytes_sent(omt_send_t* instance) {
    if (!instance) return 0;
    auto* sender = reinterpret_cast<omt::Sender*>(instance);
    return sender->getStats().totalBytesSent;
}

int omt_send_get_profile(omt_send_t* instance) {
    if (!instance) return 0;
    auto* sender = reinterpret_cast<omt::Sender*>(instance);
    return sender->getCurrentProfile();
}

void omt_send_set_quality(omt_send_t* instance, OMTQuality quality) {
    if (!instance) return;
    auto* sender = reinterpret_cast<omt::Sender*>(instance);
    
    omt::Quality q;
    switch (quality) {
        case OMTQuality_Low: q = omt::Quality::Low; break;
        case OMTQuality_Medium: q = omt::Quality::Medium; break;
        case OMTQuality_High: q = omt::Quality::High; break;
        default: q = omt::Quality::Default; break;
    }
    
    sender->setQuality(q);
}

int64_t omt_send_get_recent_drops_and_reset(omt_send_t* instance) {
    // Not implemented in new modular design - would need to track in Sender
    // For now, return 0 (no recent drops tracked)
    return 0;
}

} // extern "C"

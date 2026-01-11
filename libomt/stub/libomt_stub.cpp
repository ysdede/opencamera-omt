#include "../libomt.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <android/log.h>

#define LOG_TAG "libomt-stub"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

// Dummy implementations

extern "C" {

char** omt_discovery_getaddresses(int * count) {
    LOGI("omt_discovery_getaddresses called");
    if (count) *count = 0;
    return nullptr;
}

omt_receive_t* omt_receive_create(const char* address, OMTFrameType frameTypes, OMTPreferredVideoFormat format, OMTReceiveFlags flags) {
    LOGI("omt_receive_create called: %s", address);
    return (omt_receive_t*)malloc(sizeof(omt_receive_t));
}

void omt_receive_destroy(omt_receive_t* instance) {
    LOGI("omt_receive_destroy called");
    if (instance) free(instance);
}

OMTMediaFrame* omt_receive(omt_receive_t* instance, OMTFrameType frameTypes, int timeoutMilliseconds) {
    return nullptr;
}

int omt_receive_send(omt_receive_t* instance, OMTMediaFrame* frame) {
    return 0;
}

void omt_receive_settally(omt_receive_t* instance, OMTTally* tally) {}

int omt_receive_gettally(omt_send_t* instance, int timeoutMilliseconds, OMTTally* tally) {
    return 0;
}

void omt_receive_setflags(omt_receive_t* instance, OMTReceiveFlags flags) {}
void omt_receive_setsuggestedquality(omt_receive_t* instance, OMTQuality quality) {}
void omt_receive_getsenderinformation(omt_receive_t* instance, OMTSenderInfo* info) {}
void omt_receive_getvideostatistics(omt_receive_t* instance, OMTStatistics* stats) {}
void omt_receive_getaudiostatistics(omt_receive_t* instance, OMTStatistics* stats) {}

// Sender
omt_send_t* omt_send_create(const char* name, OMTQuality quality) {
    LOGI("omt_send_create called: %s", name);
    return (omt_send_t*)malloc(sizeof(omt_send_t));
}

void omt_send_setsenderinformation(omt_send_t* instance, OMTSenderInfo* info) {}
void omt_send_addconnectionmetadata(omt_send_t* instance, const char* metadata) {}
void omt_send_clearconnectionmetadata(omt_send_t* instance) {}
void omt_send_setredirect(omt_send_t* instance, const char* newAddress) {}

int omt_send_getaddress(omt_send_t* instance, char* address, int maxLength) {
    if (address && maxLength > 0) address[0] = '\0';
    return 0;
}

void omt_send_destroy(omt_send_t* instance) {
    LOGI("omt_send_destroy called");
    if (instance) free(instance);
}

int omt_send(omt_send_t* instance, OMTMediaFrame* frame) {
    // LOGI("omt_send called"); // Don't log every frame
    return 1;
}

int omt_send_connections(omt_send_t* instance) {
    return 1; // Fake 1 connection
}

OMTMediaFrame* omt_send_receive(omt_send_t* instance, int timeoutMilliseconds) {
    return nullptr;
}

int omt_send_gettally(omt_send_t* instance, int timeoutMilliseconds, OMTTally* tally) {
    return 0;
}

void omt_send_getvideostatistics(omt_send_t* instance, OMTStatistics* stats) {}
void omt_send_getaudiostatistics(omt_send_t* instance, OMTStatistics* stats) {}

// Logging & Settings
void omt_setloggingfilename(const char* filename) {}
int omt_settings_get_string(const char* name, char* value, int maxLength) { return 0; }
void omt_settings_set_string(const char* name, const char* value) {}
int omt_settings_get_integer(const char* name) { return 0; }
void omt_settings_set_integer(const char* name, int value) {}

}

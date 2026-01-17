/**
 * OMTAsyncPool.cpp - Async Send Buffer Pool Implementation
 * Port of libomtnet/src/OMTSocketAsyncPool.cs
 */

#include "OMTAsyncPool.h"

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "omt-asyncpool"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) 
#define LOGD(...)
#endif

namespace omt {

AsyncPool::AsyncPool(int count, size_t bufferSize) {
    pool_.reserve(count);
    for (int i = 0; i < count; i++) {
        pool_.push_back(new AsyncBuffer(bufferSize));
    }
    LOGI("AsyncPool: created %d buffers of %zu bytes", count, bufferSize);
}

AsyncPool::~AsyncPool() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto* buf : pool_) {
        delete buf;
    }
    pool_.clear();
}

AsyncBuffer* AsyncPool::acquire() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto* buf : pool_) {
        if (!buf->inUse) {
            buf->inUse = true;
            buf->length = 0;
            return buf;
        }
    }
    return nullptr;  // All buffers in use
}

void AsyncPool::release(AsyncBuffer* buf) {
    if (buf) {
        std::lock_guard<std::mutex> lock(mutex_);
        buf->inUse = false;
        buf->length = 0;
    }
}

int AsyncPool::availableCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    int count = 0;
    for (const auto* buf : pool_) {
        if (!buf->inUse) count++;
    }
    return count;
}

} // namespace omt

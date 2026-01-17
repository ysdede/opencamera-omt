/**
 * OMTAsyncPool.h - Async Send Buffer Pool
 * Port of libomtnet/src/OMTSocketAsyncPool.cs
 */

#ifndef OMT_ASYNC_POOL_H
#define OMT_ASYNC_POOL_H

#include <vector>
#include <mutex>
#include <cstdint>
#include "OMTConstants.h"

namespace omt {

/**
 * AsyncBuffer - A pre-allocated buffer for async sending
 * Equivalent to SocketAsyncEventArgs in .NET
 */
class AsyncBuffer {
public:
    std::vector<uint8_t> data;
    size_t length = 0;
    bool inUse = false;
    
    explicit AsyncBuffer(size_t bufferSize) : data(bufferSize) {}
    
    void resize(size_t newSize) {
        if (data.size() < newSize) {
            data.resize(newSize);
        }
    }
    
    void reset() {
        length = 0;
        inUse = false;
    }
};

/**
 * OMTAsyncPool - Pool of async send buffers
 * Port of OMTSocketAsyncPool from libomtnet
 * 
 * Design Pattern: Object Pool
 * - Pre-allocates fixed number of buffers
 * - Returns nullptr when exhausted (caller should drop frame)
 * - Thread-safe via mutex
 */
class AsyncPool {
public:
    /**
     * Create pool with specified count and buffer size
     * @param count Number of buffers (default: NETWORK_ASYNC_COUNT = 4)
     * @param bufferSize Initial size per buffer (default: 1MB)
     */
    AsyncPool(int count = Constants::NETWORK_ASYNC_COUNT, 
              size_t bufferSize = Constants::NETWORK_ASYNC_BUFFER_AV);
    
    ~AsyncPool();
    
    // Non-copyable
    AsyncPool(const AsyncPool&) = delete;
    AsyncPool& operator=(const AsyncPool&) = delete;
    
    /**
     * Get an available buffer from the pool
     * @return Buffer pointer, or nullptr if all buffers in use
     */
    AsyncBuffer* acquire();
    
    /**
     * Return a buffer to the pool
     */
    void release(AsyncBuffer* buf);
    
    /**
     * Get count of available buffers
     */
    int availableCount() const;
    
    /**
     * Get total pool size
     */
    int totalCount() const { return static_cast<int>(pool_.size()); }

private:
    std::vector<AsyncBuffer*> pool_;
    mutable std::mutex mutex_;
};

} // namespace omt

#endif // OMT_ASYNC_POOL_H

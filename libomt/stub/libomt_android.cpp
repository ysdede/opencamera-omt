/**
 * libomt_android.cpp - OMT Sender for Android (Ported from libomtnet)
 * 
 * This is a C++ port of the .NET libomtnet sender functionality.
 * Uses async send pool pattern matching the original OMTSocketAsyncPool.
 */

#include "../libomt.h"
#include "../../libvmx/src/vmxcodec.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#include <android/log.h>
#include <errno.h>

#define LOG_TAG "libomt-android-v6"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

// =============================================================
// Constants (from OMTConstants.cs)
// =============================================================
namespace OMTConstants {
    const int NETWORK_SEND_BUFFER = 65536;
    const int NETWORK_ASYNC_COUNT = 4;           // Pool size for async buffers
    const int NETWORK_ASYNC_BUFFER_AV = 1048576; // 1MB per buffer
    const int VIDEO_MAX_SIZE = 10485760;         // 10MB max frame
    const int METADATA_MAX_COUNT = 60;
}

// =============================================================
// Metadata Constants (from OMTMetadata.cs)
// =============================================================
namespace OMTMetadataConstants {
    // Subscription commands - receivers send these to subscribe to frame types
    const char* CHANNEL_SUBSCRIBE_VIDEO = "<OMTSubscribe Video=\"true\" />";
    const char* CHANNEL_SUBSCRIBE_AUDIO = "<OMTSubscribe Audio=\"true\" />";
    const char* CHANNEL_SUBSCRIBE_METADATA = "<OMTSubscribe Metadata=\"true\" />";
    
    // Preview mode commands
    const char* CHANNEL_PREVIEW_VIDEO_ON = "<OMTSettings Preview=\"true\" />";
    const char* CHANNEL_PREVIEW_VIDEO_OFF = "<OMTSettings Preview=\"false\" />";
    
    // Tally commands
    const char* TALLY_PREVIEW = "<OMTTally Preview=\"true\" Program==\"false\" />";
    const char* TALLY_PROGRAM = "<OMTTally Preview=\"false\" Program==\"true\" />";
    const char* TALLY_PREVIEWPROGRAM = "<OMTTally Preview=\"true\" Program==\"true\" />";
    const char* TALLY_NONE = "<OMTTally Preview=\"false\" Program==\"false\" />";
    
    // Quality suggestion prefix
    const char* SUGGESTED_QUALITY_PREFIX = "<OMTSettings Quality=";
}

// Subscription flags (matching OMTFrameType)
enum OMTSubscription : uint8_t {
    SUB_NONE = 0,
    SUB_VIDEO = 1,
    SUB_AUDIO = 2,
    SUB_METADATA = 4
};

// Quality levels for dynamic adjustment
enum OMTQualityLevel {
    QUALITY_DEFAULT = 0,
    QUALITY_LOW = 1,
    QUALITY_MEDIUM = 2,
    QUALITY_HIGH = 3
};

// =============================================================
// OMT Protocol Structures
// =============================================================
#pragma pack(push, 1)

struct OMTFrameHeaderInternal {
    uint8_t Version;       // = 1
    uint8_t FrameType;
    int64_t Timestamp;
    uint16_t MetadataLength;
    int32_t DataLength;    // Includes extended header + payload
};

struct OMTVideoHeaderInternal {
    int32_t Codec;
    int32_t Width;
    int32_t Height;
    int32_t FrameRateN;
    int32_t FrameRateD;
    float AspectRatio;
    int32_t Flags;
    int32_t ColorSpace;
};

const int HEADER_SIZE = 16;
const int EXT_HEADER_VIDEO_SIZE = 32;

#pragma pack(pop)

// =============================================================
// OMTSocketAsyncPool - Port of .NET async send pool
// =============================================================

/**
 * AsyncBuffer - A pre-allocated buffer for async sending
 * Equivalent to SocketAsyncEventArgs in .NET
 */
struct AsyncBuffer {
    std::vector<uint8_t> data;
    size_t length;
    bool inUse;
    
    AsyncBuffer(int bufferSize) : data(bufferSize), length(0), inUse(false) {}
};

/**
 * OMTSocketAsyncPool - Pool of async send buffers
 * Port of libomtnet/OMTSocketAsyncPool.cs
 */
class OMTSocketAsyncPool {
private:
    std::vector<AsyncBuffer*> pool;
    std::mutex poolMutex;
    int bufferSize;
    
public:
    OMTSocketAsyncPool(int count, int bufSize) : bufferSize(bufSize) {
        for (int i = 0; i < count; i++) {
            pool.push_back(new AsyncBuffer(bufSize));
        }
        LOGI("OMTSocketAsyncPool: created %d buffers of %d bytes", count, bufSize);
    }
    
    ~OMTSocketAsyncPool() {
        std::lock_guard<std::mutex> lock(poolMutex);
        for (auto* buf : pool) {
            delete buf;
        }
        pool.clear();
    }
    
    /**
     * Get an available buffer from the pool
     * Returns nullptr if all buffers are in use (frame should be dropped)
     */
    AsyncBuffer* GetBuffer() {
        std::lock_guard<std::mutex> lock(poolMutex);
        for (auto* buf : pool) {
            if (!buf->inUse) {
                buf->inUse = true;
                return buf;
            }
        }
        return nullptr;  // All buffers in use
    }
    
    /**
     * Return a buffer to the pool after send completes
     */
    void ReturnBuffer(AsyncBuffer* buf) {
        if (buf) {
            std::lock_guard<std::mutex> lock(poolMutex);
            buf->inUse = false;
            buf->length = 0;
        }
    }
    
    /**
     * Resize buffer if needed (for large frames)
     */
    void Resize(AsyncBuffer* buf, size_t length) {
        if (buf && buf->data.size() < length) {
            buf->data.resize(length);
            LOGD("OMTSocketAsyncPool: resized buffer to %zu bytes", length);
        }
    }
    
    int GetAvailableCount() {
        std::lock_guard<std::mutex> lock(poolMutex);
        int count = 0;
        for (auto* buf : pool) {
            if (!buf->inUse) count++;
        }
        return count;
    }
};

// =============================================================
// ClientConnection - Port of OMTChannel with async sending
// =============================================================

class ClientConnection {
public:
    int socketFd;
    struct sockaddr_in address;
    std::atomic<bool> connected;
    
    // Async send infrastructure
    OMTSocketAsyncPool* sendPool;
    std::queue<AsyncBuffer*> pendingQueue;
    std::mutex queueMutex;
    std::condition_variable queueCV;
    std::thread* senderThread;
    std::thread* receiverThread;  // For processing incoming metadata
    std::atomic<bool> running;
    
    // Subscriptions (from OMTChannel.cs line 49)
    std::atomic<uint8_t> subscriptions{SUB_NONE};
    
    // Quality negotiation (from OMTChannel.cs line 56)
    std::atomic<int> suggestedQuality{QUALITY_DEFAULT};
    
    // Tally state (from OMTChannel.cs line 52)
    std::atomic<bool> tallyPreview{false};
    std::atomic<bool> tallyProgram{false};
    
    // Preview mode
    std::atomic<bool> previewMode{false};
    
    // Statistics
    std::atomic<int64_t> framesSent{0};
    std::atomic<int64_t> framesDropped{0};
    std::atomic<int64_t> bytesSent{0};

    ClientConnection(int fd, struct sockaddr_in addr, int sendBufferSize = 512 * 1024) 
        : socketFd(fd), address(addr), connected(true), running(true) {
        
        // Set non-blocking for recv (we'll use blocking send in sender thread)
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        
        // Disable Nagle (TCP_NODELAY) - critical for low latency
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        
        // Set send buffer
        setSendBuffer(sendBufferSize);
        
        // Enable TCP keepalive
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
        
        // Create async send pool (4 buffers like .NET)
        sendPool = new OMTSocketAsyncPool(OMTConstants::NETWORK_ASYNC_COUNT, 
                                          OMTConstants::NETWORK_ASYNC_BUFFER_AV);
        
        // Start sender thread (handles async sends)
        senderThread = new std::thread(&ClientConnection::SenderLoop, this);
        
        // Start receiver thread (handles incoming metadata/subscriptions)
        receiverThread = new std::thread(&ClientConnection::ReceiverLoop, this);
        
        // Send OMTInfo Metadata
        const char* infoXml = "<OMTInfo ProductName=\"OMT Android Sender\" Manufacturer=\"Open Media Transport\" Version=\"1.0\" />";
        SendMetadataSync(infoXml);
        
        char clientIP[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(addr.sin_addr), clientIP, INET_ADDRSTRLEN);
        LOGI("ClientConnection: connected %s:%d, pool=%d buffers", 
             clientIP, ntohs(addr.sin_port), OMTConstants::NETWORK_ASYNC_COUNT);
    }
    
    // Check if client subscribed to video
    bool IsVideoSubscribed() const {
        return (subscriptions.load() & SUB_VIDEO) != 0;
    }
    
    // Check if client subscribed to audio  
    bool IsAudioSubscribed() const {
        return (subscriptions.load() & SUB_AUDIO) != 0;
    }
    
    // Check if client subscribed to metadata
    bool IsMetadataSubscribed() const {
        return (subscriptions.load() & SUB_METADATA) != 0;
    }
    
    // Get suggested quality from receiver
    int GetSuggestedQuality() const {
        return suggestedQuality.load();
    }
    
    void setSendBuffer(int size) {
        setsockopt(socketFd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
        
        int actualBuf = 0;
        socklen_t optlen = sizeof(actualBuf);
        getsockopt(socketFd, SOL_SOCKET, SO_SNDBUF, &actualBuf, &optlen);
        LOGI("Client SO_SNDBUF set to %d bytes (requested %d)", actualBuf, size);
    }

    ~ClientConnection() {
        LOGI("ClientConnection: destroying");
        running = false;
        connected = false;
        
        // Close socket first to unblock recv() in receiver thread
        if (socketFd > 0) {
            shutdown(socketFd, SHUT_RDWR);
        }
        
        queueCV.notify_all();
        
        // Join receiver thread
        if (receiverThread && receiverThread->joinable()) {
            receiverThread->join();
            delete receiverThread;
        }
        
        // Join sender thread
        if (senderThread && senderThread->joinable()) {
            senderThread->join();
            delete senderThread;
        }
        
        // Clear pending queue
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            while (!pendingQueue.empty()) {
                sendPool->ReturnBuffer(pendingQueue.front());
                pendingQueue.pop();
            }
        }
        
        if (sendPool) {
            delete sendPool;
        }
        
        if (socketFd > 0) {
            close(socketFd);
        }
        LOGI("ClientConnection: destroyed, sent=%lld, dropped=%lld", 
             (long long)framesSent.load(), (long long)framesDropped.load());
    }

    /**
     * Synchronous send for metadata (used during handshake)
     */
    bool SendMetadataSync(const char* xml) {
        int len = strlen(xml);
        int totalLen = len + 1;
        
        OMTFrameHeaderInternal header;
        header.Version = 1;
        header.FrameType = 1; // Metadata
        header.Timestamp = 0;
        header.MetadataLength = (uint16_t)totalLen;
        header.DataLength = totalLen;
        
        // Make socket blocking temporarily for sync send
        int flags = fcntl(socketFd, F_GETFL, 0);
        fcntl(socketFd, F_SETFL, flags & ~O_NONBLOCK);
        
        ssize_t s1 = send(socketFd, &header, sizeof(header), MSG_NOSIGNAL);
        
        std::vector<char> buf(totalLen);
        memcpy(buf.data(), xml, len);
        buf[len] = 0;
        
        ssize_t s2 = send(socketFd, buf.data(), totalLen, MSG_NOSIGNAL);
        
        // Restore non-blocking
        fcntl(socketFd, F_SETFL, flags | O_NONBLOCK);
        
        return (s1 > 0 && s2 == totalLen);
    }
    
    void DrainInput() {
        char buf[1024];
        while(true) {
            ssize_t r = recv(socketFd, buf, sizeof(buf), MSG_DONTWAIT);
            if (r <= 0) break;
        }
    }

    /**
     * Queue a frame for async sending
     * Port of OMTChannel.Send()
     * Returns bytes queued (0 if dropped)
     */
    int SendAsync(const void* headerData, size_t headerLen,
                  const void* extHeaderData, size_t extHeaderLen,
                  const void* payloadData, size_t payloadLen) {
        
        if (!connected) return 0;
        
        size_t totalLen = headerLen + extHeaderLen + payloadLen;
        
        // Check size limit
        if (totalLen > OMTConstants::VIDEO_MAX_SIZE) {
            framesDropped++;
            LOGE("SendAsync: frame too large %zu > %d", totalLen, OMTConstants::VIDEO_MAX_SIZE);
            return 0;
        }
        
        // Get a buffer from the pool
        AsyncBuffer* buf = sendPool->GetBuffer();
        if (!buf) {
            // All buffers in use - drop frame (same as .NET behavior)
            framesDropped++;
            static int dropLogCount = 0;
            if (dropLogCount++ % 30 == 0) {
                LOGD("SendAsync: pool exhausted, dropping frame (total dropped: %lld)", 
                     (long long)framesDropped.load());
            }
            return 0;
        }
        
        // Resize if needed
        sendPool->Resize(buf, totalLen);
        
        // Copy data to buffer
        size_t offset = 0;
        memcpy(buf->data.data() + offset, headerData, headerLen);
        offset += headerLen;
        memcpy(buf->data.data() + offset, extHeaderData, extHeaderLen);
        offset += extHeaderLen;
        memcpy(buf->data.data() + offset, payloadData, payloadLen);
        buf->length = totalLen;
        
        // Queue for async send
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            pendingQueue.push(buf);
        }
        queueCV.notify_one();
        
        return (int)totalLen;
    }
    
private:
    /**
     * Sender thread - processes queued buffers
     * This is the async send equivalent
     */
    void SenderLoop() {
        LOGI("SenderLoop: started");
        
        // Make socket blocking for sender thread
        int flags = fcntl(socketFd, F_GETFL, 0);
        fcntl(socketFd, F_SETFL, flags & ~O_NONBLOCK);
        
        while (running && connected) {
            AsyncBuffer* buf = nullptr;
            
            // Wait for work
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                queueCV.wait_for(lock, std::chrono::milliseconds(100), [this] {
                    return !pendingQueue.empty() || !running || !connected;
                });
                
                if (!running || !connected) break;
                if (pendingQueue.empty()) continue;
                
                buf = pendingQueue.front();
                pendingQueue.pop();
            }
            
            if (!buf) continue;
            
            // Blocking send (receiver thread handles incoming data)
            bool success = SendAllBlocking(buf->data.data(), buf->length);
            
            if (success) {
                framesSent++;
                bytesSent += buf->length;
            } else {
                if (!connected) {
                    LOGI("SenderLoop: connection closed");
                }
            }
            
            // Return buffer to pool
            sendPool->ReturnBuffer(buf);
        }
        
        LOGI("SenderLoop: exiting");
    }
    
    bool SendAllBlocking(const uint8_t* data, size_t length) {
        size_t remaining = length;
        const uint8_t* ptr = data;
        
        while (remaining > 0 && connected) {
            ssize_t sent = send(socketFd, ptr, remaining, MSG_NOSIGNAL);
            
            if (sent > 0) {
                ptr += sent;
                remaining -= sent;
            } else if (sent < 0) {
                if (errno == EINTR) continue;  // Interrupted, retry
                
                LOGE("SendAllBlocking: error %d (%s)", errno, strerror(errno));
                connected = false;
                return false;
            } else {
                LOGI("SendAllBlocking: connection closed");
                connected = false;
                return false;
            }
        }
        
        return remaining == 0;
    }
    
    /**
     * Receiver thread - processes incoming metadata from receivers
     * Port of OMTChannel receive logic + ProcessMetadata()
     */
    void ReceiverLoop() {
        LOGI("ReceiverLoop: started");
        
        std::vector<uint8_t> recvBuffer(65536);
        size_t bufferPos = 0;
        
        while (running && connected) {
            // Non-blocking recv with select timeout
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(socketFd, &readfds);
            
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 100000;  // 100ms timeout
            
            int sel = select(socketFd + 1, &readfds, nullptr, nullptr, &tv);
            
            if (sel < 0) {
                if (errno == EINTR) continue;
                LOGE("ReceiverLoop: select error %d", errno);
                connected = false;
                break;
            }
            
            if (sel == 0) continue;  // Timeout, check running flag
            
            ssize_t received = recv(socketFd, recvBuffer.data() + bufferPos, 
                                    recvBuffer.size() - bufferPos, 0);
            
            if (received <= 0) {
                if (received == 0) {
                    LOGI("ReceiverLoop: connection closed by peer");
                } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    LOGE("ReceiverLoop: recv error %d (%s)", errno, strerror(errno));
                }
                connected = false;
                break;
            }
            
            bufferPos += received;
            
            // Process complete frames
            while (bufferPos >= HEADER_SIZE) {
                OMTFrameHeaderInternal* header = (OMTFrameHeaderInternal*)recvBuffer.data();
                
                // Validate header
                if (header->Version != 1) {
                    LOGE("ReceiverLoop: invalid protocol version %d", header->Version);
                    connected = false;
                    break;
                }
                
                int totalFrameLen = HEADER_SIZE + header->DataLength;
                
                if ((size_t)totalFrameLen > bufferPos) {
                    // Incomplete frame, wait for more data
                    break;
                }
                
                // Process metadata frames (FrameType=1)
                if (header->FrameType == 1 && header->DataLength > 0) {
                    char* xmlData = (char*)(recvBuffer.data() + HEADER_SIZE);
                    std::string xml(xmlData, header->DataLength - 1); // Exclude null terminator
                    ProcessMetadata(xml);
                }
                
                // Shift buffer
                if ((size_t)totalFrameLen < bufferPos) {
                    memmove(recvBuffer.data(), recvBuffer.data() + totalFrameLen, 
                            bufferPos - totalFrameLen);
                }
                bufferPos -= totalFrameLen;
            }
        }
        
        LOGI("ReceiverLoop: exiting");
    }
    
    /**
     * Process incoming metadata commands
     * Port of OMTChannel.ProcessMetadata() from libomtnet
     */
    void ProcessMetadata(const std::string& xml) {
        // Subscription commands
        if (xml == OMTMetadataConstants::CHANNEL_SUBSCRIBE_VIDEO) {
            subscriptions.fetch_or(SUB_VIDEO);
            LOGI("Client subscribed to VIDEO");
            return;
        }
        if (xml == OMTMetadataConstants::CHANNEL_SUBSCRIBE_AUDIO) {
            subscriptions.fetch_or(SUB_AUDIO);
            LOGI("Client subscribed to AUDIO");
            return;
        }
        if (xml == OMTMetadataConstants::CHANNEL_SUBSCRIBE_METADATA) {
            subscriptions.fetch_or(SUB_METADATA);
            LOGI("Client subscribed to METADATA");
            return;
        }
        
        // Tally commands
        if (xml == OMTMetadataConstants::TALLY_PREVIEWPROGRAM) {
            tallyPreview = true; tallyProgram = true;
            LOGI("Tally: Preview+Program");
            return;
        }
        if (xml == OMTMetadataConstants::TALLY_PROGRAM) {
            tallyPreview = false; tallyProgram = true;
            LOGI("Tally: Program");
            return;
        }
        if (xml == OMTMetadataConstants::TALLY_PREVIEW) {
            tallyPreview = true; tallyProgram = false;
            LOGI("Tally: Preview");
            return;
        }
        if (xml == OMTMetadataConstants::TALLY_NONE) {
            tallyPreview = false; tallyProgram = false;
            LOGI("Tally: None");
            return;
        }
        
        // Preview mode
        if (xml == OMTMetadataConstants::CHANNEL_PREVIEW_VIDEO_ON) {
            previewMode = true;
            LOGI("Preview mode: ON");
            return;
        }
        if (xml == OMTMetadataConstants::CHANNEL_PREVIEW_VIDEO_OFF) {
            previewMode = false;
            LOGI("Preview mode: OFF");
            return;
        }
        
        // Quality suggestion (e.g., <OMTSettings Quality="High" />)
        if (xml.find(OMTMetadataConstants::SUGGESTED_QUALITY_PREFIX) == 0) {
            if (xml.find("\"Low\"") != std::string::npos) {
                suggestedQuality = QUALITY_LOW;
                LOGI("Suggested quality: LOW");
            } else if (xml.find("\"Medium\"") != std::string::npos) {
                suggestedQuality = QUALITY_MEDIUM;
                LOGI("Suggested quality: MEDIUM");
            } else if (xml.find("\"High\"") != std::string::npos) {
                suggestedQuality = QUALITY_HIGH;
                LOGI("Suggested quality: HIGH");
            } else {
                suggestedQuality = QUALITY_DEFAULT;
                LOGI("Suggested quality: DEFAULT");
            }
            return;
        }
        
        // Log unknown metadata for debugging
        LOGD("ReceiverLoop: unknown metadata: %s", xml.c_str());
    }
};

// =============================================================
// OmtSenderContext - Port of OMTSend
// =============================================================

class OmtSenderContext {
public:
    std::vector<ClientConnection*> clients;
    std::mutex clientsMutex;
    int serverSocket = -1;
    std::thread* acceptThread = nullptr;
    std::atomic<bool> running;
    
    // VMX Codec
    VMX_INSTANCE* vmxInstance = nullptr;
    int width = 0;
    int height = 0;
    VMX_PROFILE profile = VMX_PROFILE_DEFAULT;
    std::vector<uint8_t> encodeBuffer;
    
    // Adaptive send buffer size
    std::atomic<int> optimalSendBuffer{512 * 1024};
    
    // Aggregated statistics
    std::atomic<int64_t> totalFramesSent{0};
    std::atomic<int64_t> totalFramesDropped{0};
    std::atomic<int64_t> totalBytesSent{0};
    std::atomic<int64_t> recentDrops{0};
    
    static int calculateOptimalBuffer(int w, int h, VMX_PROFILE prof) {
        int rawSize = w * h * 3 / 2;
        
        float compressionRatio;
        switch (prof) {
            case VMX_PROFILE_OMT_HQ: compressionRatio = 0.25f; break;
            case VMX_PROFILE_OMT_SQ: compressionRatio = 0.18f; break;
            case VMX_PROFILE_OMT_LQ: compressionRatio = 0.12f; break;
            default: compressionRatio = 0.20f; break;
        }
        
        int estimatedFrameSize = (int)(rawSize * compressionRatio);
        int bufferSize = (int)(estimatedFrameSize * 1.5f) + 4096;
        
        if (bufferSize < 256 * 1024) bufferSize = 256 * 1024;
        if (bufferSize > 2 * 1024 * 1024) bufferSize = 2 * 1024 * 1024;
        
        LOGI("Adaptive buffer: %dx%d, profile=%d, estFrame=%dKB, buffer=%dKB",
             w, h, (int)prof, estimatedFrameSize/1024, bufferSize/1024);
        
        return bufferSize;
    }

    OmtSenderContext(int q) {
        running = true;
        
        if (q <= 0) {
            profile = VMX_PROFILE_OMT_SQ;
            LOGI("OmtSenderContext: Default -> VMX_PROFILE_OMT_SQ");
        } else if (q <= 1) {
            profile = VMX_PROFILE_OMT_LQ;
            LOGI("OmtSenderContext: Low -> VMX_PROFILE_OMT_LQ");
        } else if (q <= 50) {
            profile = VMX_PROFILE_OMT_SQ;
            LOGI("OmtSenderContext: Medium -> VMX_PROFILE_OMT_SQ");
        } else {
            profile = VMX_PROFILE_OMT_HQ;
            LOGI("OmtSenderContext: High -> VMX_PROFILE_OMT_HQ");
        }
    }

    ~OmtSenderContext() {
        LOGI("OmtSenderContext: destruction started");
        running = false;
        
        if (serverSocket > 0) {
            shutdown(serverSocket, SHUT_RDWR);
            close(serverSocket);
        }
        
        if (acceptThread && acceptThread->joinable()) {
            acceptThread->join();
            delete acceptThread;
        }
        
        {
            std::lock_guard<std::mutex> lock(clientsMutex);
            for (auto c : clients) delete c;
            clients.clear();
        }
        
        if (vmxInstance) {
            VMX_Destroy(vmxInstance);
            vmxInstance = nullptr;
        }
        
        LOGI("OmtSenderContext: destruction complete");
    }

    void StartServer(int port) {
        serverSocket = socket(AF_INET, SOCK_STREAM, 0);
        if (serverSocket < 0) {
            LOGE("Failed to create socket");
            return;
        }

        int one = 1;
        setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        if (bind(serverSocket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            LOGE("Failed to bind port %d", port);
            return;
        }

        listen(serverSocket, 5);
        LOGI("OMT Server listening on port %d (pool size: %d)", port, OMTConstants::NETWORK_ASYNC_COUNT);

        acceptThread = new std::thread(&OmtSenderContext::AcceptLoop, this);
    }

    void AcceptLoop() {
        while (running) {
            struct sockaddr_in cli_addr;
            socklen_t clilen = sizeof(cli_addr);
            int newsockfd = accept(serverSocket, (struct sockaddr*)&cli_addr, &clilen);
            
            if (newsockfd < 0) {
                if (errno == EINVAL || errno == EBADF) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            LOGI("New connection from %s:%d", inet_ntoa(cli_addr.sin_addr), ntohs(cli_addr.sin_port));
            
            ClientConnection* client = new ClientConnection(newsockfd, cli_addr, optimalSendBuffer.load());
            
            std::lock_guard<std::mutex> lock(clientsMutex);
            clients.push_back(client);
        }
    }
    
    void UpdateAggregatedStats() {
        int64_t sent = 0, dropped = 0, bytes = 0;
        std::lock_guard<std::mutex> lock(clientsMutex);
        for (auto* c : clients) {
            sent += c->framesSent.load();
            dropped += c->framesDropped.load();
            bytes += c->bytesSent.load();
        }
        totalFramesSent = sent;
        totalFramesDropped = dropped;
        totalBytesSent = bytes;
    }
};

// =============================================================
// C API Implementation
// =============================================================

extern "C" {

char** omt_discovery_getaddresses(int * count) {
    if (count) *count = 0;
    return nullptr;
}

omt_receive_t* omt_receive_create(const char* address, OMTFrameType frameTypes, OMTPreferredVideoFormat format, OMTReceiveFlags flags) { return nullptr; }
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

// Sender Implementation
omt_send_t* omt_send_create(const char* name, OMTQuality quality) {
    LOGI("omt_send_create: name=%s, quality=%d", name, (int)quality);
    OmtSenderContext* ctx = new OmtSenderContext((int)quality);
    ctx->StartServer(6400);
    return (omt_send_t*)ctx;
}

void omt_send_destroy(omt_send_t* instance) {
    if (instance) {
        OmtSenderContext* ctx = (OmtSenderContext*)instance;
        delete ctx;
    }
}

int omt_send(omt_send_t* instance, OMTMediaFrame* frame) {
    if (!instance || !frame) return 0;
    OmtSenderContext* ctx = (OmtSenderContext*)instance;
    
    if (frame->Type != OMTFrameType_Video) return 0;
    
    bool hasClients = false;
    {
        std::lock_guard<std::mutex> lock(ctx->clientsMutex);
        
        // Remove disconnected clients
        auto it = ctx->clients.begin();
        while (it != ctx->clients.end()) {
            if (!(*it)->connected) {
                LOGI("Removing disconnected client");
                delete *it;
                it = ctx->clients.erase(it);
            } else {
                ++it;
            }
        }
        
        hasClients = !ctx->clients.empty();
    }
    if (!hasClients) return 1;
    
    // Initialize Codec if needed
    if (!ctx->vmxInstance || ctx->width != frame->Width || ctx->height != frame->Height) {
        if (ctx->vmxInstance) VMX_Destroy(ctx->vmxInstance);
        ctx->width = frame->Width;
        ctx->height = frame->Height;
        
        VMX_SIZE size = { frame->Width, frame->Height };
        VMX_COLORSPACE cspace = VMX_COLORSPACE_BT709;
        
        ctx->vmxInstance = VMX_Create(size, ctx->profile, cspace);
        LOGI("VMX codec created: %dx%d, Profile=%d", frame->Width, frame->Height, (int)ctx->profile);
        
        int max_size = frame->Width * frame->Height * 4;
        if (max_size < 1024*1024) max_size = 1024*1024;
        ctx->encodeBuffer.resize(max_size);
        
        int newBuffer = OmtSenderContext::calculateOptimalBuffer(frame->Width, frame->Height, ctx->profile);
        ctx->optimalSendBuffer = newBuffer;
        
        std::lock_guard<std::mutex> lock(ctx->clientsMutex);
        for (auto* c : ctx->clients) {
            c->setSendBuffer(newBuffer);
        }
    }
    
    // Encode Frame
    int interlaced = (frame->Flags & OMTVideoFlags_Interlaced) ? 1 : 0;
    VMX_ERR err = VMX_ERR_UNKNOWN;
    
    switch(frame->Codec) {
        case OMTCodec_UYVY: 
            err = VMX_EncodeUYVY(ctx->vmxInstance, (BYTE*)frame->Data, frame->Stride, interlaced); 
            break;
        case OMTCodec_YUY2: 
            err = VMX_EncodeYUY2(ctx->vmxInstance, (BYTE*)frame->Data, frame->Stride, interlaced); 
            break;
        case OMTCodec_NV12: 
             {
                 BYTE* y = (BYTE*)frame->Data;
                 BYTE* uv = y + (frame->Stride * frame->Height);
                 err = VMX_EncodeNV12(ctx->vmxInstance, y, frame->Stride, uv, frame->Stride, interlaced);
             }
             break;
        case OMTCodec_BGRA: 
            err = VMX_EncodeBGRA(ctx->vmxInstance, (BYTE*)frame->Data, frame->Stride, interlaced); 
            break;
        default: 
            LOGE("Unknown codec: %d", frame->Codec);
            return 0;
    }

    if (err != VMX_ERR_OK) {
        static int errCount = 0;
        if (errCount++ < 100) LOGE("VMX Encode failed: %d", err);
        return 0;
    }
    
    int encodedLen = VMX_SaveTo(ctx->vmxInstance, ctx->encodeBuffer.data(), ctx->encodeBuffer.size());
    
    if (encodedLen <= 0) {
        LOGE("VMX_SaveTo produced 0 bytes");
        return 0;
    }
    
    // Log every 30 frames
    static int frameCount = 0;
    static int64_t totalBytes = 0;
    totalBytes += encodedLen;
    frameCount++;
    
    if (frameCount % 30 == 0) {
        float avgBytesPerFrame = (float)totalBytes / frameCount;
        float estimatedMbps = (avgBytesPerFrame * 8.0f * frame->FrameRateN / frame->FrameRateD) / 1000000.0f;
        LOGI("Frame %d: %d bytes (avg %.0f bytes, ~%.1f Mbps @ %d fps)", 
             frameCount, encodedLen, avgBytesPerFrame, estimatedMbps, 
             frame->FrameRateN / frame->FrameRateD);
    }
    
    // Prepare headers
    OMTFrameHeaderInternal header;
    header.Version = 1;
    header.FrameType = 2; // Video
    header.Timestamp = frame->Timestamp;
    header.MetadataLength = 0; 
    header.DataLength = EXT_HEADER_VIDEO_SIZE + encodedLen;
    
    OMTVideoHeaderInternal extHeader;
    extHeader.Codec = 0x31584D56; // VMX1
    extHeader.Width = frame->Width;
    extHeader.Height = frame->Height;
    extHeader.FrameRateN = frame->FrameRateN;
    extHeader.FrameRateD = frame->FrameRateD;
    extHeader.AspectRatio = frame->AspectRatio;
    extHeader.Flags = frame->Flags;
    extHeader.ColorSpace = frame->ColorSpace;
    
    // Send to all clients using async pool
    // Port of OMTChannel.Send() subscription check
    int totalSent = 0;
    {
        std::lock_guard<std::mutex> lock(ctx->clientsMutex);
        for (auto* c : ctx->clients) {
            if (c->connected) {
                // Check subscription (from OMTChannel.cs line 195)
                // For now, always send to connected clients (Android camera app case)
                // In proper OMT protocol, client must subscribe first
                // But many clients auto-subscribe on connect
                if (c->IsVideoSubscribed() || c->subscriptions.load() == SUB_NONE) {
                    // If no subscriptions yet, assume client wants everything (vMix behavior)
                    int sent = c->SendAsync(&header, sizeof(header),
                                            &extHeader, sizeof(extHeader),
                                            ctx->encodeBuffer.data(), encodedLen);
                    if (sent > 0) {
                        totalSent += sent;
                    }
                }
            }
        }
    }
    
    // Update aggregated stats periodically
    if (frameCount % 30 == 0) {
        ctx->UpdateAggregatedStats();
    }
    
    return totalSent > 0 ? encodedLen : 0;
}

int omt_send_connections(omt_send_t* instance) {
    if (!instance) return 0;
    OmtSenderContext* ctx = (OmtSenderContext*)instance;
    std::lock_guard<std::mutex> lock(ctx->clientsMutex);
    return (int)ctx->clients.size();
}

void omt_send_setsenderinformation(omt_send_t* instance, OMTSenderInfo* info) {}
void omt_send_addconnectionmetadata(omt_send_t* instance, const char* metadata) {}
void omt_send_clearconnectionmetadata(omt_send_t* instance) {}
void omt_send_setredirect(omt_send_t* instance, const char* newAddress) {}
int omt_send_getaddress(omt_send_t* instance, char* address, int maxLength) { return 0; }
OMTMediaFrame* omt_send_receive(omt_send_t* instance, int timeoutMilliseconds) { return nullptr; }
int omt_send_gettally(omt_send_t* instance, int timeoutMilliseconds, OMTTally* tally) { return 0; }
void omt_send_getvideostatistics(omt_send_t* instance, OMTStatistics* stats) {}
void omt_send_getaudiostatistics(omt_send_t* instance, OMTStatistics* stats) {}
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
    OmtSenderContext* ctx = (OmtSenderContext*)instance;
    ctx->UpdateAggregatedStats();
    return ctx->totalFramesSent.load();
}

int64_t omt_send_get_frames_dropped(omt_send_t* instance) {
    if (!instance) return 0;
    OmtSenderContext* ctx = (OmtSenderContext*)instance;
    ctx->UpdateAggregatedStats();
    return ctx->totalFramesDropped.load();
}

int64_t omt_send_get_recent_drops_and_reset(omt_send_t* instance) {
    if (!instance) return 0;
    OmtSenderContext* ctx = (OmtSenderContext*)instance;
    
    // Calculate recent drops from all clients
    int64_t totalDrops = 0;
    {
        std::lock_guard<std::mutex> lock(ctx->clientsMutex);
        for (auto* c : ctx->clients) {
            totalDrops += c->framesDropped.load();
        }
    }
    
    int64_t recent = totalDrops - ctx->recentDrops.exchange(totalDrops);
    return recent > 0 ? recent : 0;
}

int64_t omt_send_get_bytes_sent(omt_send_t* instance) {
    if (!instance) return 0;
    OmtSenderContext* ctx = (OmtSenderContext*)instance;
    ctx->UpdateAggregatedStats();
    return ctx->totalBytesSent.load();
}

}

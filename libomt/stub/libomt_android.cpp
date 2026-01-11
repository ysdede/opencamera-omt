#include "../libomt.h"
#include "../../libvmx/src/vmxcodec.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <algorithm>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#include <android/log.h>
#include <errno.h>

#define LOG_TAG "libomt-android-v5"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

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

// We only implement Video Extended Header for now
const int HEADER_SIZE = 16;
const int EXT_HEADER_VIDEO_SIZE = 32;

#pragma pack(pop)

// =============================================================
// Helper Classes
// =============================================================

class ClientConnection {
public:
    int socketFd;
    struct sockaddr_in address;
    bool connected;

    ClientConnection(int fd, struct sockaddr_in addr, int sendBufferSize = 512 * 1024) 
        : socketFd(fd), address(addr), connected(true) {
        // Set non-blocking
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        
        // Disable Nagle (TCP_NODELAY) - critical for low latency
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        
        // Set send buffer (adaptive or default)
        setSendBuffer(sendBufferSize);
        
        // Enable TCP keepalive to detect dead connections
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
        
        // Send OMTInfo Metadata to satisfy protocol handshake
        const char* infoXml = "<OMTInfo ProductName=\"OMT Android Sender\" Manufacturer=\"Open Media Transport\" Version=\"1.0\" />";
        SendMetadata(infoXml);
    }
    
    void setSendBuffer(int size) {
        setsockopt(socketFd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
        
        // Get actual buffer size for logging
        int actualBuf = 0;
        socklen_t optlen = sizeof(actualBuf);
        getsockopt(socketFd, SOL_SOCKET, SO_SNDBUF, &actualBuf, &optlen);
        LOGI("Client SO_SNDBUF set to %d bytes (requested %d)", actualBuf, size);
    }

    ~ClientConnection() {
        if (connected && socketFd > 0) {
            close(socketFd);
        }
    }

    bool SendMetadata(const char* xml) {
        int len = strlen(xml);
        int totalLen = len + 1; // Include null
        
        OMTFrameHeaderInternal header;
        header.Version = 1;
        header.FrameType = 1; // Metadata
        header.Timestamp = 0;
        header.MetadataLength = (uint16_t)totalLen;
        header.DataLength = totalLen; // No extended header for Metadata
        
        // Send Header
        ssize_t s1 = send(socketFd, &header, sizeof(header), MSG_NOSIGNAL);
        if (s1 <= 0) return false;
        
        // Send XML + Null
        std::vector<char> buf(totalLen);
        memcpy(buf.data(), xml, len);
        buf[len] = 0;
        
        ssize_t s2 = send(socketFd, buf.data(), totalLen, MSG_NOSIGNAL);
        return (s2 == totalLen);
    }
    
    void DrainInput() {
        char buf[1024];
        // Read until empty
        while(true) {
            ssize_t r = recv(socketFd, buf, sizeof(buf), MSG_DONTWAIT);
            if (r <= 0) break; 
            // We ignore incoming commands like Subscribe for now, assuming "Always On"
        }
    }

    bool SendAll(const void* data, size_t length) {
        if (!connected) return false;
        
        // Drain input logic to keep TCP window open
        DrainInput();
        
        const uint8_t* ptr = (const uint8_t*)data;
        size_t remaining = length;
        
        while (remaining > 0 && connected) {
            ssize_t sent = send(socketFd, ptr, remaining, MSG_NOSIGNAL);
            
            if (sent > 0) {
                ptr += sent;
                remaining -= sent;
            } else if (sent < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Buffer full - drop frame immediately, no retries
                    return false;
                }
                // Real error - disconnect
                LOGE("SendAll: socket error %d (%s), disconnecting", errno, strerror(errno));
                connected = false;
                return false;
            } else {
                // sent == 0 - connection closed
                LOGI("SendAll: connection closed by peer");
                connected = false;
                return false;
            }
        }
        
        return remaining == 0;
    }
};

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
    
    // Adaptive send buffer size (calculated from resolution + overhead)
    std::atomic<int> optimalSendBuffer{512 * 1024}; // Default 512KB until we know frame size
    
    // Frame statistics (atomic for thread safety)
    std::atomic<int64_t> totalFramesSent{0};
    std::atomic<int64_t> totalFramesDropped{0};
    std::atomic<int64_t> totalBytesSent{0};
    std::atomic<int64_t> recentDrops{0};  // Drops in last reporting period
    std::atomic<int64_t> lastStatsResetTime{0};
    
    // Calculate optimal buffer size: ~1.5x estimated max frame size
    // VMX compression ratio is roughly 15-25% of raw YUV for HQ
    static int calculateOptimalBuffer(int w, int h, VMX_PROFILE prof) {
        // Raw YUV420 size = w * h * 1.5
        int rawSize = w * h * 3 / 2;
        
        // Compression ratio depends on profile:
        // HQ: ~20-25% of raw, SQ: ~15-18%, LQ: ~10-12%
        float compressionRatio;
        switch (prof) {
            case VMX_PROFILE_OMT_HQ: compressionRatio = 0.25f; break;
            case VMX_PROFILE_OMT_SQ: compressionRatio = 0.18f; break;
            case VMX_PROFILE_OMT_LQ: compressionRatio = 0.12f; break;
            default: compressionRatio = 0.20f; break;
        }
        
        int estimatedFrameSize = (int)(rawSize * compressionRatio);
        
        // Buffer = 1.5x frame + overhead for headers
        int bufferSize = (int)(estimatedFrameSize * 1.5f) + 4096;
        
        // Clamp to reasonable range: 256KB - 2MB
        if (bufferSize < 256 * 1024) bufferSize = 256 * 1024;
        if (bufferSize > 2 * 1024 * 1024) bufferSize = 2 * 1024 * 1024;
        
        LOGI("Adaptive buffer: %dx%d, profile=%d, estFrame=%dKB, buffer=%dKB",
             w, h, (int)prof, estimatedFrameSize/1024, bufferSize/1024);
        
        return bufferSize;
    }

    OmtSenderContext(int q) {
        running = true;
        // Use OMT-specific profiles defined in vmxcodec.h
        // These profiles likely handle bitrate targeting internally
        
        if (q <= 0) {
            profile = VMX_PROFILE_OMT_SQ; // Default (Medium)
            LOGI("OmtSenderContext: Default -> VMX_PROFILE_OMT_SQ");
        } else if (q <= 1) {
            profile = VMX_PROFILE_OMT_LQ; // LOW
            LOGI("OmtSenderContext: Low -> VMX_PROFILE_OMT_LQ");
        } else if (q <= 50) {
            profile = VMX_PROFILE_OMT_SQ; // MEDIUM
            LOGI("OmtSenderContext: Medium -> VMX_PROFILE_OMT_SQ");
        } else {
            profile = VMX_PROFILE_OMT_HQ; // HIGH
            LOGI("OmtSenderContext: High -> VMX_PROFILE_OMT_HQ");
        }
    }

    ~OmtSenderContext() {
        LOGI("OmtSenderContext: safe destruction started");
        running = false;
        if (serverSocket > 0) {
            shutdown(serverSocket, SHUT_RDWR);
            close(serverSocket);
        }
        
        if (acceptThread && acceptThread->joinable()) {
            LOGI("OmtSenderContext: joining accept thread");
            acceptThread->join();
            delete acceptThread;
        }
        
        {
            LOGI("OmtSenderContext: deleting clients");
            std::lock_guard<std::mutex> lock(clientsMutex);
            for (auto c : clients) delete c;
            clients.clear();
        }
        
        if (vmxInstance) {
            LOGI("OmtSenderContext: destroying VMX instance");
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
        int reuse = setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (reuse < 0) LOGE("setsockopt failed");

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
        LOGI("OMT Server listening on port %d", port);

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
            
            // Use adaptive buffer size
            ClientConnection* client = new ClientConnection(newsockfd, cli_addr, optimalSendBuffer.load());
            
            std::lock_guard<std::mutex> lock(clientsMutex);
            clients.push_back(client);
        }
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
    LOGI("omt_send_create: name=%s, OMTQuality=%d", name, (int)quality);
    OmtSenderContext* ctx = new OmtSenderContext((int)quality);
    ctx->StartServer(6400); // 6400 is default OMT port
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
        
        // Use the selected OMT Profile
        ctx->vmxInstance = VMX_Create(size, ctx->profile, cspace);
        
        // DO NOT call VMX_SetQuality - let the profile manage bitrate targeting
        LOGI("VMX codec created: %dx%d, Profile=%d", frame->Width, frame->Height, (int)ctx->profile);
        
        int max_size = frame->Width * frame->Height * 4;
        if (max_size < 1024*1024) max_size = 1024*1024;
        ctx->encodeBuffer.resize(max_size);
        
        // Calculate and apply optimal send buffer based on resolution
        int newBuffer = OmtSenderContext::calculateOptimalBuffer(frame->Width, frame->Height, ctx->profile);
        ctx->optimalSendBuffer = newBuffer;
        
        // Update existing clients with optimal buffer
        {
            std::lock_guard<std::mutex> lock(ctx->clientsMutex);
            for (auto* c : ctx->clients) {
                c->setSendBuffer(newBuffer);
            }
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
             // NV12 requires two planes, assuming they are contiguous for now: Y then UV
             // frame->Data points to Y, calculate UV offset
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
    } else {
        static int frameCount = 0;
        static int64_t totalBytes = 0;
        totalBytes += encodedLen;
        frameCount++;
        
        // Log every 30 frames (~1 sec @ 30fps)
        if (frameCount % 30 == 0) {
            // Calculate bitrate: bytes * 8 bits * fps / 1000000 = Mbps
            float avgBytesPerFrame = (float)totalBytes / frameCount;
            float estimatedMbps = (avgBytesPerFrame * 8.0f * frame->FrameRateN / frame->FrameRateD) / 1000000.0f;
            LOGI("Frame %d: %d bytes (avg %.0f bytes, ~%.1f Mbps @ %d fps, Profile=%d)", 
                 frameCount, encodedLen, avgBytesPerFrame, estimatedMbps, 
                 frame->FrameRateN / frame->FrameRateD, (int)ctx->profile);
        }
    }
    
    if (encodedLen > 0) {
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
        
        bool anyDropped = false;
        std::lock_guard<std::mutex> lock(ctx->clientsMutex);
        for (auto it = ctx->clients.begin(); it != ctx->clients.end(); ) {
            ClientConnection* c = *it;
            bool ok = true;
            ok &= c->SendAll(&header, sizeof(header));
            ok &= c->SendAll(&extHeader, sizeof(extHeader));
            ok &= c->SendAll(ctx->encodeBuffer.data(), encodedLen);
            
            if (!ok) {
                if (!c->connected) {
                     char clientIP[INET_ADDRSTRLEN];
                     inet_ntop(AF_INET, &(c->address.sin_addr), clientIP, INET_ADDRSTRLEN);
                     LOGI("Client disconnected: %s:%d (removing from list)", clientIP, ntohs(c->address.sin_port));
                     delete c;
                     it = ctx->clients.erase(it);
                     continue;
                }
                // Frame dropped but client still connected - track it
                anyDropped = true;
            } else {
                ctx->totalBytesSent += encodedLen;
            }
            ++it;
        }
        
        ctx->totalFramesSent++;
        if (anyDropped) {
            ctx->totalFramesDropped++;
            ctx->recentDrops++;
        }
        
        return encodedLen;
    }

    return 0;
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
// Extended Statistics API (for UI feedback)
// =============================================================

// Get total frames sent
int64_t omt_send_get_frames_sent(omt_send_t* instance) {
    if (!instance) return 0;
    OmtSenderContext* ctx = (OmtSenderContext*)instance;
    return ctx->totalFramesSent.load();
}

// Get total frames dropped
int64_t omt_send_get_frames_dropped(omt_send_t* instance) {
    if (!instance) return 0;
    OmtSenderContext* ctx = (OmtSenderContext*)instance;
    return ctx->totalFramesDropped.load();
}

// Get recent drops and reset counter (for periodic UI updates)
int64_t omt_send_get_recent_drops_and_reset(omt_send_t* instance) {
    if (!instance) return 0;
    OmtSenderContext* ctx = (OmtSenderContext*)instance;
    return ctx->recentDrops.exchange(0);
}

// Get total bytes sent
int64_t omt_send_get_bytes_sent(omt_send_t* instance) {
    if (!instance) return 0;
    OmtSenderContext* ctx = (OmtSenderContext*)instance;
    return ctx->totalBytesSent.load();
}

}

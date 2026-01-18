/**
 * OMTSender.cpp - OMT Video Sender Implementation
 * Port of libomtnet/src/OMTSend.cs
 */

#include "OMTSender.h"
#include "OMTProtocol.h"
#include "../../libvmx/src/vmxcodec.h"

#include <cstring>
#include <algorithm>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sstream>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "omt-sender"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) 
#define LOGE(...)
#define LOGW(...)
#define LOGD(...)
#endif

namespace omt {

Sender::Sender(const SenderConfig& config) : config_(config) {
    LOGI("Sender: creating '%s' quality=%d", config_.name.c_str(), static_cast<int>(config_.quality));
    currentProfile_ = selectProfile(config_.quality);
}

Sender::~Sender() {
    LOGI("Sender: destroying");
    stop();
    destroyEncoder();
}

bool Sender::start() {
    if (running_.load()) return true;
    
    serverSocket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket_ < 0) {
        LOGE("Sender: failed to create socket");
        return false;
    }
    
    int one = 1;
    setsockopt(serverSocket_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(config_.port);
    
    if (bind(serverSocket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOGE("Sender: failed to bind port %d", config_.port);
        close(serverSocket_);
        serverSocket_ = -1;
        return false;
    }
    
    listen(serverSocket_, 5);
    running_ = true;
    
    LOGI("Sender: listening on port %d", config_.port);
    
    acceptThread_ = std::thread(&Sender::acceptLoop, this);
    return true;
}

void Sender::stop() {
    if (!running_.load()) return;
    
    running_ = false;
    
    if (serverSocket_ > 0) {
        shutdown(serverSocket_, SHUT_RDWR);
        close(serverSocket_);
        serverSocket_ = -1;
    }
    
    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }
    
    // Disconnect all channels
    {
        std::lock_guard<std::mutex> lock(channelsMutex_);
        channels_.clear();
    }
    
    LOGI("Sender: stopped");
}

void Sender::acceptLoop() {
    LOGI("acceptLoop: started");
    
    while (running_) {
        struct sockaddr_in clientAddr{};
        socklen_t addrLen = sizeof(clientAddr);
        
        int clientFd = accept(serverSocket_, 
                              reinterpret_cast<sockaddr*>(&clientAddr), 
                              &addrLen);
        
        if (clientFd < 0) {
            if (errno == EINVAL || errno == EBADF) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        
        char clientIP[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, sizeof(clientIP));
        LOGI("New connection from %s:%d", clientIP, ntohs(clientAddr.sin_port));
        
        // Get current optimal buffer size
        int bufferSize = config_.sendBufferSize;
        {
            std::lock_guard<std::mutex> lock(encoderMutex_);
            if (currentWidth_ > 0 && currentHeight_ > 0) {
                bufferSize = calculateOptimalBuffer(currentWidth_, currentHeight_, currentProfile_);
            }
        }
        
        auto channel = std::make_unique<Channel>(clientFd, clientAddr, bufferSize, this);
        
        // Send configured sender info immediately
        channel->sendMetadataSync(config_.senderInfoXml.c_str());

        if (onConnect_) {
            onConnect_(channel.get());
        }
        
        addChannel(std::move(channel));
    }
    
    LOGI("acceptLoop: exiting");
}

void Sender::addChannel(std::unique_ptr<Channel> channel) {
    std::lock_guard<std::mutex> lock(channelsMutex_);
    channels_.push_back(std::move(channel));
}

void Sender::removeChannel(Channel* channel) {
    std::lock_guard<std::mutex> lock(channelsMutex_);
    channels_.erase(
        std::remove_if(channels_.begin(), channels_.end(),
            [channel](const std::unique_ptr<Channel>& c) {
                return c.get() == channel;
            }),
        channels_.end());
}

void Sender::cleanupDisconnected() {
    std::lock_guard<std::mutex> lock(channelsMutex_);
    channels_.erase(
        std::remove_if(channels_.begin(), channels_.end(),
            [](const std::unique_ptr<Channel>& c) {
                return !c->isConnected();
            }),
        channels_.end());
}

int Sender::connectionCount() const {
    std::lock_guard<std::mutex> lock(channelsMutex_);
    return static_cast<int>(channels_.size());
}

SenderStats Sender::getStats() {
    cleanupDisconnected();
    
    SenderStats stats;
    stats.totalEncodedBytes = totalEncodedBytes_.load();
    
    std::lock_guard<std::mutex> lock(channelsMutex_);
    stats.connectionCount = static_cast<int>(channels_.size());
    
    for (const auto& ch : channels_) {
        auto chStats = ch->getStats();
        stats.totalFramesSent += chStats.framesSent;
        stats.totalFramesDropped += chStats.framesDropped;
        stats.totalBytesSent += chStats.bytesSent;
    }
    
    return stats;
}

int Sender::selectProfile(Quality quality) const {
    switch (quality) {
        case Quality::Low: return VMX_PROFILE_OMT_LQ;
        case Quality::Medium: return VMX_PROFILE_OMT_SQ;
        case Quality::High: return VMX_PROFILE_OMT_HQ;
        default: return VMX_PROFILE_OMT_SQ;
    }
}

void Sender::setQuality(Quality quality) {
    LOGI("Updating quality to %d", static_cast<int>(quality));
    
    std::lock_guard<std::mutex> lock(encoderMutex_);
    config_.quality = quality;
    
    if (quality != Quality::Default) {
        int newProfile = selectProfile(quality);
        if (newProfile != currentProfile_) {
            currentProfile_ = newProfile;
            destroyEncoder();
            LOGI("Profile changed to %d on manual request", newProfile);
        }
    }
}

int Sender::calculateOptimalBuffer(int width, int height, int profile) {
    int rawSize = width * height * 3 / 2;
    
    float compressionRatio;
    switch (profile) {
        case VMX_PROFILE_OMT_HQ: compressionRatio = 0.25f; break;
        case VMX_PROFILE_OMT_SQ: compressionRatio = 0.18f; break;
        case VMX_PROFILE_OMT_LQ: compressionRatio = 0.12f; break;
        default: compressionRatio = 0.20f; break;
    }
    
    int bufferSize = static_cast<int>(rawSize * compressionRatio * 1.5f) + 4096;
    
    if (bufferSize < 256 * 1024) bufferSize = 256 * 1024;
    if (bufferSize > 2 * 1024 * 1024) bufferSize = 2 * 1024 * 1024;
    
    return bufferSize;
}

void Sender::createEncoder(int width, int height, int frameRate, int colorSpace) {
    std::lock_guard<std::mutex> lock(encoderMutex_);
    
    if (vmxInstance_ && currentWidth_ == width && currentHeight_ == height) {
        return;  // Already have correct encoder. Recreated via destroyEncoder() if profile changed.
    }
    
    destroyEncoder();
    
    VMX_SIZE size = {width, height};
    VMX_COLORSPACE cspace = static_cast<VMX_COLORSPACE>(colorSpace);
    
    vmxInstance_ = VMX_Create(size, static_cast<VMX_PROFILE>(currentProfile_), cspace);
    currentWidth_ = width;
    currentHeight_ = height;
    
    int maxSize = width * height * 4;
    if (maxSize < 1024 * 1024) maxSize = 1024 * 1024;
    encodeBuffer_.resize(maxSize);
    
    LOGI("Encoder created: %dx%d, profile=%d", width, height, static_cast<int>(currentProfile_));
}

void Sender::destroyEncoder() {
    if (vmxInstance_) {
        VMX_Destroy(vmxInstance_);
        vmxInstance_ = nullptr;
    }
    currentWidth_ = 0;
    currentHeight_ = 0;
}

int Sender::encodeFrame(const MediaFrame& frame) {
    std::lock_guard<std::mutex> lock(encoderMutex_);
    
    if (!vmxInstance_) return 0;
    
    int interlaced = (frame.flags & VideoFlags::Interlaced) ? 1 : 0;
    VMX_ERR err = VMX_ERR_UNKNOWN;
    
    BYTE* data = static_cast<BYTE*>(frame.data);
    
    // Encode based on input codec
    switch (frame.codec) {
        case Codec::UYVY:
            err = VMX_EncodeUYVY(vmxInstance_, data, frame.stride, interlaced);
            break;
        case Codec::YUY2:
            err = VMX_EncodeYUY2(vmxInstance_, data, frame.stride, interlaced);
            break;
        case 0x3231564E:  // NV12
            {
                BYTE* y = data;
                BYTE* uv = data + (frame.stride * frame.height);
                err = VMX_EncodeNV12(vmxInstance_, y, frame.stride, uv, frame.stride, interlaced);
            }
            break;
        case Codec::BGRA:
            err = VMX_EncodeBGRA(vmxInstance_, data, frame.stride, interlaced);
            break;
        default:
            LOGE("Unknown codec: 0x%08X", frame.codec);
            return 0;
    }
    
    if (err != VMX_ERR_OK) {
        LOGE("VMX encode failed: %d", err);
        return 0;
    }
    
    int encodedLen = VMX_SaveTo(vmxInstance_, encodeBuffer_.data(), encodeBuffer_.size());
    return encodedLen;
}

int Sender::send(const MediaFrame& frame) {
    // Cleanup disconnected first
    cleanupDisconnected();
    
    if (connectionCount() == 0) return 1;  // No clients, but not an error
    
    // Create/update encoder
    createEncoder(frame.width, frame.height, 
                  frame.frameRateN / frame.frameRateD, 
                  frame.colorSpace);
    
    // Encode frame
    int encodedLen = encodeFrame(frame);
    if (encodedLen <= 0) return 0;
    
    totalEncodedBytes_ += encodedLen;
    int count = frameCount_++;
    
    // Log periodically
    if (count % 30 == 0) {
        float avgBytes = static_cast<float>(totalEncodedBytes_.load()) / (count + 1);
        float mbps = (avgBytes * 8.0f * frame.frameRateN / frame.frameRateD) / 1000000.0f;
        LOGI("Frame %d: %d bytes (~%.1f Mbps)", count, encodedLen, mbps);
        checkCongestion();
    }
    
    // Build headers
    FrameHeader header{};
    header.version = 1;
    header.frameType = static_cast<uint8_t>(FrameType::Video);
    header.timestamp = frame.timestamp;
    header.metadataLength = 0;
    header.dataLength = VideoExtHeader::SIZE + encodedLen;
    
    VideoExtHeader extHeader{};
    extHeader.codec = Codec::VMX1;
    extHeader.width = frame.width;
    extHeader.height = frame.height;
    extHeader.frameRateN = frame.frameRateN;
    extHeader.frameRateD = frame.frameRateD;
    extHeader.aspectRatio = frame.aspectRatio;
    extHeader.flags = frame.flags;
    extHeader.colorSpace = frame.colorSpace;
    
    // Send to all subscribed clients
    int totalSent = 0;
    {
        std::lock_guard<std::mutex> lock(channelsMutex_);
        for (const auto& ch : channels_) {
            if (!ch->isConnected()) continue;
            
            // Only send if client has subscribed to video (per OMT protocol)
            if (!ch->isVideoSubscribed()) continue;
            
            int sent = ch->sendAsync(&header, sizeof(header),
                                     &extHeader, sizeof(extHeader),
                                     encodeBuffer_.data(), encodedLen);
            if (sent > 0) {
                totalSent += sent;
            }
        }
    }
    
    return totalSent > 0 ? encodedLen : 0;
}

void Sender::sendMetadata(const std::string& xml) {
    std::lock_guard<std::mutex> lock(channelsMutex_);
    for (const auto& ch : channels_) {
        if (ch->isConnected()) {
            ch->sendMetadataSync(xml.c_str());
        }
    }
}

Quality Sender::getBestSuggestedQuality() const {
    Quality best = Quality::Default;
    
    std::lock_guard<std::mutex> lock(channelsMutex_);
    for (const auto& ch : channels_) {
        if (ch->isConnected()) {
            Quality q = ch->getSuggestedQuality();
            if (static_cast<int>(q) > static_cast<int>(best)) {
                best = q;
            }
        }
    }
    
    return best;
}

bool Sender::hasTallyPreview() const {
    std::lock_guard<std::mutex> lock(channelsMutex_);
    for (const auto& ch : channels_) {
        if (ch->isConnected() && ch->isTallyPreview()) {
            return true;
        }
    }
    return false;
}

bool Sender::hasTallyProgram() const {
    std::lock_guard<std::mutex> lock(channelsMutex_);
    for (const auto& ch : channels_) {
        if (ch->isConnected() && ch->isTallyProgram()) {
            return true;
        }
    }
    return false;
}

// IChannelListener implementation

void Sender::onDisconnected(Channel* channel) {
    LOGI("Channel disconnected");
    if (onDisconnect_) {
        onDisconnect_(channel);
    }
    // Cleanup will happen on next send or getStats call
}

void Sender::onTallyChanged(Channel* channel) {
    LOGI("Tally changed: preview=%d, program=%d",
         channel->isTallyPreview(), channel->isTallyProgram());
}

void Sender::onQualityChanged(Channel* channel) {
    LOGI("Quality suggested: %d", static_cast<int>(channel->getSuggestedQuality()));
    
    // If using auto quality, update encoder
    if (config_.quality == Quality::Default) {
        Quality best = getBestSuggestedQuality();
        if (best != Quality::Default) {
            std::lock_guard<std::mutex> lock(encoderMutex_);
            int newProfile = selectProfile(best);
            if (newProfile != currentProfile_) {
                currentProfile_ = newProfile;
                // Encoder will be recreated on next send
                destroyEncoder();
                LOGI("Profile changed to %d based on receiver suggestion", 
                     static_cast<int>(newProfile));
            }
        }
    }
}

void Sender::setSenderInfo(const std::string& productName, const std::string& manufacturer) {
    std::ostringstream ss;
    ss << "<OMTInfo ProductName=\"" << productName << "\" Manufacturer=\"" << manufacturer << "\" Version=\"1.0\" />";
    config_.senderInfoXml = ss.str();
    
    // Broadcast to existing channels
    sendMetadata(config_.senderInfoXml);
}

void Sender::checkCongestion() {
    SenderStats stats = getStats();
    if (stats.totalFramesSent < 30) return; // Too early

    double dropRate = (double)stats.totalFramesDropped / (double)(stats.totalFramesSent + stats.totalFramesDropped);
    
    if (dropRate > 0.05) { // > 5% drops
        LOGW("High drop rate detected: %.2f%%", dropRate * 100.0);
        
        // Downgrade profile if possible
        if (config_.quality == Quality::Default || config_.quality == Quality::High || config_.quality == Quality::Medium) {
            std::lock_guard<std::mutex> lock(encoderMutex_);
            int current = currentProfile_;
            int lower = VMX_PROFILE_OMT_LQ;
            
            if (current == VMX_PROFILE_OMT_HQ) lower = VMX_PROFILE_OMT_SQ;
            else if (current == VMX_PROFILE_OMT_SQ) lower = VMX_PROFILE_OMT_LQ;
            
            if (lower != current) {
                currentProfile_ = lower;
                destroyEncoder();
                LOGI("Congestion Control: Downgraded profile to %d", currentProfile_);
            }
        }
    }
}

// ... existing code ...

} // namespace omt

/**
 * OMTDiscovery.cpp - mDNS/DNS-SD Discovery Implementation
 * Port of libomtnet/src/OMTDiscovery.cs
 * 
 * On Android, actual mDNS operations are done via Java NsdManager.
 * This C++ code manages the entry list and provides the interface.
 */

#include "OMTDiscovery.h"
#include <algorithm>
#include <chrono>
#include <cstring>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "omt-discovery"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...)
#define LOGE(...)
#define LOGD(...)
#endif

namespace omt {

// Singleton instance
static Discovery* s_instance = nullptr;
static std::mutex s_instanceMutex;

Discovery& Discovery::getInstance() {
    std::lock_guard<std::mutex> lock(s_instanceMutex);
    if (!s_instance) {
        s_instance = new Discovery();
    }
    return *s_instance;
}

Discovery::Discovery() {
    LOGI("Discovery: created");
}

Discovery::~Discovery() {
    stop();
    LOGI("Discovery: destroyed");
}

bool Discovery::start() {
    if (status_.load() == DiscoveryStatus::Running) {
        return true;
    }
    
    status_ = DiscoveryStatus::Starting;
    
    // On Android, the Java layer handles NsdManager.discoverServices()
    // This is called from Java after NSD is initialized
    
    status_ = DiscoveryStatus::Running;
    LOGI("Discovery: started");
    return true;
}

void Discovery::stop() {
    if (status_.load() == DiscoveryStatus::Stopped) {
        return;
    }
    
    // Unregister all services
    {
        std::lock_guard<std::mutex> lock(mutex_);
        registeredServices_.clear();
        entries_.clear();
    }
    
    status_ = DiscoveryStatus::Stopped;
    LOGI("Discovery: stopped");
}

bool Discovery::registerService(const std::string& name, int port) {
    if (name.empty() || port <= 0) {
        LOGE("Discovery: invalid service name or port");
        return false;
    }
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // Check if already registered
        auto it = std::find(registeredServices_.begin(), registeredServices_.end(), name);
        if (it != registeredServices_.end()) {
            LOGD("Discovery: service '%s' already registered", name.c_str());
            return true;
        }
        
        registeredServices_.push_back(name);
    }
    
    // On Android, the Java layer calls NsdManager.registerService()
    // We add the entry as local
    DiscoveryEntry entry;
    entry.name = name;
    entry.fullName = name + "." + ServiceType::OMT + "." + ServiceType::LOCAL_DOMAIN;
    entry.port = port;
    entry.isLocal = true;
    entry.discoveredAt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    addOrUpdateEntry(entry);
    
    LOGI("Discovery: registerService '%s' port=%d", name.c_str(), port);
    return true;
}

bool Discovery::unregisterService(const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = std::find(registeredServices_.begin(), registeredServices_.end(), name);
        if (it == registeredServices_.end()) {
            return false;
        }
        
        registeredServices_.erase(it);
    }
    
    removeEntry(name);
    
    LOGI("Discovery: unregisterService '%s'", name.c_str());
    return true;
}

std::vector<std::string> Discovery::getAddresses() {
    removeExpiredEntries();
    
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> addresses;
    addresses.reserve(entries_.size());
    
    for (const auto& entry : entries_) {
        // Format: omt://name:port or omt://name (if port is default)
        std::string addr = std::string(Constants::URL_PREFIX) + entry.name;
        if (entry.port != Constants::NETWORK_PORT_START && entry.port > 0) {
            addr += ":" + std::to_string(entry.port);
        }
        addresses.push_back(addr);
    }
    
    std::sort(addresses.begin(), addresses.end());
    return addresses;
}

std::vector<DiscoveryEntry> Discovery::getEntries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_;
}

const DiscoveryEntry* Discovery::findEntry(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : entries_) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

void Discovery::addOrUpdateEntry(const DiscoveryEntry& entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Find existing
    for (auto& e : entries_) {
        if (e.name == entry.name) {
            // Update
            e.hostAddress = entry.hostAddress;
            e.port = entry.port;
            if (!entry.isLocal) {
                e.discoveredAt = entry.discoveredAt;
            }
            LOGD("Discovery: updated '%s' -> %s:%d", 
                 entry.name.c_str(), entry.hostAddress.c_str(), entry.port);
            return;
        }
    }
    
    // Add new
    entries_.push_back(entry);
    LOGD("Discovery: added '%s' -> %s:%d", 
         entry.name.c_str(), entry.hostAddress.c_str(), entry.port);
}

void Discovery::removeEntry(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(),
            [&name](const DiscoveryEntry& e) { return e.name == name; }),
        entries_.end());
}

void Discovery::removeExpiredEntries() {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(),
            [now](const DiscoveryEntry& e) {
                return e.expiresAt > 0 && e.expiresAt < now;
            }),
        entries_.end());
}

// Callbacks from JNI/Java layer

void Discovery::onServiceDiscovered(const std::string& name, const std::string& host, int port) {
    LOGI("Discovery: onServiceDiscovered '%s' -> %s:%d", name.c_str(), host.c_str(), port);
    
    DiscoveryEntry entry;
    entry.name = name;
    entry.fullName = name + "." + ServiceType::OMT + "." + ServiceType::LOCAL_DOMAIN;
    entry.hostAddress = host;
    entry.port = port;
    entry.isLocal = false;
    entry.discoveredAt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    // Set expiry to 60 seconds from now (if not refreshed)
    entry.expiresAt = entry.discoveredAt + 60000;
    
    addOrUpdateEntry(entry);
    
    if (listener_) {
        listener_->onServiceFound(entry);
    }
}

void Discovery::onServiceLost(const std::string& name) {
    LOGI("Discovery: onServiceLost '%s'", name.c_str());
    
    removeEntry(name);
    
    if (listener_) {
        listener_->onServiceLost(name);
    }
}

void Discovery::onRegistered(const std::string& name) {
    LOGI("Discovery: onRegistered '%s'", name.c_str());
    
    if (listener_) {
        listener_->onRegistrationSuccess(name);
    }
}

void Discovery::onRegistrationFailed(const std::string& name, int errorCode) {
    LOGE("Discovery: onRegistrationFailed '%s' error=%d", name.c_str(), errorCode);
    
    // Remove from registered list
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find(registeredServices_.begin(), registeredServices_.end(), name);
        if (it != registeredServices_.end()) {
            registeredServices_.erase(it);
        }
    }
    
    if (listener_) {
        listener_->onRegistrationFailed(name, errorCode);
    }
}

} // namespace omt

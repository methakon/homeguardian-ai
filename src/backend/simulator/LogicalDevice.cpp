#include "LogicalDevice.h"

#include <algorithm>
#include <stdexcept>

namespace homeguardian {

LogicalDevice::LogicalDevice(std::string id, LogicalDeviceConfig cfg)
    : id_(std::move(id)), config_(std::move(cfg)) {
    if (id_.empty()) {
        throw std::invalid_argument("logical device id cannot be empty");
    }
    if (config_.max_event_history == 0) {
        config_.max_event_history = 1;  // keep history bounded and non-zero
    }
}

void LogicalDevice::initialize() {
    std::lock_guard<std::mutex> lk(m_);
    if (has_error_) {
        throw std::runtime_error("logical device in error state; close before re-init");
    }
    if (state_ == LogicalDeviceState::Idle) {
        state_ = LogicalDeviceState::Initialized;
        init_count_++;
        return;
    }
    if (state_ == LogicalDeviceState::Initialized) {
        throw std::runtime_error("logical device already initialized");
    }
    throw std::runtime_error("logical device initialize requires Idle state");
}

void LogicalDevice::activate() {
    std::lock_guard<std::mutex> lk(m_);
    if (has_error_) {
        throw std::runtime_error("cannot activate logical device in error state");
    }
    if (state_ != LogicalDeviceState::Initialized &&
        state_ != LogicalDeviceState::Stopped) {
        throw std::runtime_error("logical device activate requires Initialized/Stopped");
    }
    state_ = LogicalDeviceState::Active;
    activate_count_++;
}

void LogicalDevice::stop() {
    std::lock_guard<std::mutex> lk(m_);
    if (state_ == LogicalDeviceState::Active) {
        state_ = LogicalDeviceState::Stopped;
        stop_count_++;
    }
    // Safe no-op from any non-Active state.
}

void LogicalDevice::close() {
    std::lock_guard<std::mutex> lk(m_);
    if (state_ == LogicalDeviceState::Active) {
        stop_count_++;
    }
    state_ = LogicalDeviceState::Idle;
    has_error_ = false;
    released_ = true;
    close_count_++;
    event_history_.clear();  // release history on close
}

void LogicalDevice::fail() {
    std::lock_guard<std::mutex> lk(m_);
    if (state_ == LogicalDeviceState::Active) {
        stop_count_++;
    }
    state_ = LogicalDeviceState::Error;
    has_error_ = true;
}

void LogicalDevice::recover() {
    std::lock_guard<std::mutex> lk(m_);
    if (state_ == LogicalDeviceState::Active) {
        stop_count_++;
    }
    state_ = LogicalDeviceState::Idle;
    has_error_ = false;
    close_count_++;
    event_history_.clear();
}

LogicalDeviceState LogicalDevice::state() const {
    std::lock_guard<std::mutex> lk(m_);
    return state_;
}

void LogicalDevice::record_event(Event ev) {
    std::lock_guard<std::mutex> lk(m_);
    while (event_history_.size() >= config_.max_event_history) {
        event_history_.pop_front();  // drop oldest -> bounded
    }
    event_history_.push_back(std::move(ev));
}

size_t LogicalDevice::event_history_size() const {
    std::lock_guard<std::mutex> lk(m_);
    return event_history_.size();
}

std::vector<Event> LogicalDevice::recent_events() const {
    std::lock_guard<std::mutex> lk(m_);
    return std::vector<Event>(event_history_.begin(), event_history_.end());
}

void LogicalDevice::clear_event_history() {
    std::lock_guard<std::mutex> lk(m_);
    event_history_.clear();
}

} // namespace homeguardian

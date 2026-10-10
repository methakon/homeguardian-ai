#pragma once

// A host-side fake media device used to test the consent-gated delivery path
// without any hardware. It models the delivery-time consent check that a real
// NDK backend MUST perform: a frame/sample is only handed to processing if
// recheck_and_enforce() has returned allowed since the last delivery.
//
// This does NOT prove real hardware enforcement. It proves the control-flow
// contract that a real backend must satisfy.

#include "IMediaDevice.h"
#include <functional>
#include <vector>

namespace homeguardian {

// A single delivered payload (an opaque, synthetic frame/sample marker).
struct FakeDelivery {
    std::string source;
    int sequence;
};

class FakeMediaDevice : public IMediaDevice {
public:
    explicit FakeMediaDevice(DeviceKind k, std::string nm)
        : kind_(k), name_(std::move(nm)) {}

    DeviceKind kind() const override { return kind_; }

    void initialize() override {
        if (state_ == DeviceState::Error) throw std::runtime_error("fake: error state");
        if (state_ != DeviceState::Idle && state_ != DeviceState::Stopped)
            throw std::runtime_error("fake: already initialized");
        init_count_++;
        state_ = DeviceState::Initialized;
    }

    void start() override {
        if (state_ != DeviceState::Initialized && state_ != DeviceState::Stopped)
            throw std::runtime_error("fake: not startable");
        start_count_++;
        state_ = DeviceState::Capturing;
    }

    void stop() noexcept override {
        if (state_ == DeviceState::Capturing) {
            stop_count_++;
            state_ = DeviceState::Stopped;
        }
    }

    void close() noexcept override {
        // Deterministic cleanup: release any held handles, then return to Idle.
        released_ = true;
        close_count_++;
        state_ = DeviceState::Idle;
    }

    void fail() noexcept override {
        if (state_ == DeviceState::Capturing) stop_count_++;
        state_ = DeviceState::Error;
    }

    DeviceState state() const override { return state_; }
    std::string name() const override { return name_; }

    // Simulate the native capture callback producing one payload. The caller
    // (ConsentGuardedDevice) must have authorized delivery immediately before
    // this; the fake itself records what was delivered so tests can assert that
    // nothing was delivered while unauthorized.
    void deliver_payload() {
        if (state_ != DeviceState::Capturing) return;  // never deliver when not capturing
        delivered_.push_back(FakeDelivery{name_, next_sequence_++});
    }

    const std::vector<FakeDelivery>& delivered() const { return delivered_; }
    int init_count() const { return init_count_; }
    int start_count() const { return start_count_; }
    int stop_count() const { return stop_count_; }
    int close_count() const { return close_count_; }
    bool released() const { return released_; }

    ~FakeMediaDevice() override {
        if (state_ == DeviceState::Capturing) { stop_count_++; state_ = DeviceState::Stopped; }
        if (state_ != DeviceState::Idle) { close_count_++; state_ = DeviceState::Idle; }
    }

private:
    DeviceKind kind_;
    std::string name_;
    DeviceState state_ = DeviceState::Idle;
    int init_count_ = 0;
    int start_count_ = 0;
    int stop_count_ = 0;
    int close_count_ = 0;
    bool released_ = false;
    int next_sequence_ = 0;
    std::vector<FakeDelivery> delivered_;
};

} // namespace homeguardian

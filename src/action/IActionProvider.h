#pragma once

#include "action/ActionContract.h"
#include <string>

namespace homeguardian::action {

class IActionProvider {
public:
    virtual ~IActionProvider() = default;

    // The primary action type this provider handles
    virtual ActionType get_type() const = 0;

    // Human-readable identifier for diagnostics and audit
    virtual std::string get_name() const = 0;

    // Availability status: true if device / service is online and ready
    virtual bool is_online() const = 0;

    // Synchronous execution dispatch
    virtual ActionResult execute(const ActionRequest& request) = 0;

    // Cooperative cancellation hook for in-flight requests
    virtual void cancel(const std::string& request_id) = 0;
};

} // namespace homeguardian::action

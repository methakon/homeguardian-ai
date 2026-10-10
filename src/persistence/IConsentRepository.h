#pragma once

#include "IDatabase.h"
#include "core/ConsentRecord.h"
#include <vector>
#include <optional>

namespace homeguardian {

class DuplicateConsentException : public std::runtime_error {
public:
    explicit DuplicateConsentException(const std::string& msg) : std::runtime_error(msg) {}
};

class IConsentRepository {
public:
    virtual ~IConsentRepository() = default;

    virtual void save(const ConsentRecord& record) = 0;
    virtual std::optional<ConsentRecord> find_by_id(const std::string& consent_id) = 0;
    virtual std::vector<ConsentRecord> find_by_subject(const std::string& subject_scope) = 0;
    virtual std::vector<ConsentRecord> find_by_purpose(const std::string& subject_scope, const std::string& purpose) = 0;
    virtual size_t count() = 0;
    virtual size_t delete_all() = 0;
};

} // namespace homeguardian

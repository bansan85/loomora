#include "format/report.hpp"

#include <new>

#include "loomora.h"

namespace loomora {
namespace format {

void Report::add(const Issue &issue) {
    issues_.push_back(issue);
    if (issue.severity == kError)
        ++errors_;
}

void Report::add(Severity severity, const std::string &code, const std::string &entry,
                 const std::string &message, ItemKind kind, std::uint32_t id) {
    Issue issue;
    issue.severity = severity;
    issue.code = code;
    issue.entry = entry;
    issue.message = message;
    issue.item_kind = kind;
    issue.item_id = id;
    add(issue);
}

void Report::add(const FormatError &error, const std::string &code, const std::string &entry) {
    Issue issue;
    if (error.has_issue()) {
        issue = error.issue();
    } else {
        issue.severity = kError;
        issue.message = error.what();
    }
    if (issue.code.empty())
        issue.code = code;
    if (issue.entry.empty())
        issue.entry = entry;
    add(issue);
}

void Report::merge(const Report &other) {
    const std::vector<Issue> copy(other.issues_); // `other` may be *this
    issues_.insert(issues_.end(), copy.begin(), copy.end());
    errors_ += (&other == this) ? errors_ : other.errors_;
}

void Report::clear() {
    issues_.clear();
    errors_ = 0;
}

int code_for_exception(const std::exception &e) {
    if (const FormatError *fe = dynamic_cast<const FormatError *>(&e))
        return fe->code();
    if (dynamic_cast<const std::bad_alloc *>(&e) != nullptr)
        return LOOMORA_ERR_NO_MEMORY;
    return LOOMORA_ERR_INTERNAL;
}

int code_of_current_exception() noexcept {
    try {
        throw;
    } catch (const std::exception &e) {
        return code_for_exception(e);
    } catch (...) {
        return LOOMORA_ERR_INTERNAL;
    }
}

} // namespace format
} // namespace loomora

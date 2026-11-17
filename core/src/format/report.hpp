// Issues found while reading, validating or saving a .loomora file, the report that collects
// them, and the exception that carries a LOOMORA_ERR_* code through the C++ code to the C
// boundary (where it is caught and turned back into the code: no exception ever crosses the C API).
#ifndef LOOMORA_FORMAT_REPORT_HPP
#define LOOMORA_FORMAT_REPORT_HPP

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>

namespace loomora {
namespace format {

enum Severity { kWarning = 0, kError = 1 };

// The item an issue is about; the values of the non-zero kinds equal LOOMORA_KIND_*.
enum ItemKind {
    kItemNone = 0,
    kItemFrame = 1,
    kItemLayout = 2,
    kItemImage = 3,
    kItemSpool = 4,
    kItemResult = 5
};

struct Issue {
    Severity severity;
    std::string code;    // stable identifier, e.g. "zip.name", "entry.sha256", "warn.nails"
    std::string entry;   // archive entry the issue is about, "" when none
    ItemKind item_kind;  // kItemNone when the issue is not about one item
    std::uint32_t item_id;
    std::string message; // human readable, English

    Issue() : severity(kWarning), item_kind(kItemNone), item_id(0) {}
};

class FormatError;

// An ordered list of issues, with the counts the C API reports.
class Report {
  public:
    std::size_t count() const { return issues_.size(); }
    std::size_t error_count() const { return errors_; }
    std::size_t warning_count() const { return issues_.size() - errors_; }
    bool has_errors() const { return errors_ != 0; }

    const Issue &at(std::size_t index) const { return issues_.at(index); }
    const std::vector<Issue> &issues() const { return issues_; }

    void add(const Issue &issue);
    void add(Severity severity, const std::string &code, const std::string &entry,
             const std::string &message, ItemKind kind = kItemNone, std::uint32_t id = 0);
    // Adds the issue of `error` (an error issue built from its code and message when it carries
    // none), with `code` and `entry` filled in when the issue has none.
    void add(const FormatError &error, const std::string &code, const std::string &entry);
    // Appends the issues of `other`, in order (`other` may be this report).
    void merge(const Report &other);
    void clear();

  private:
    std::vector<Issue> issues_;
    std::size_t errors_ = 0;
};

// An error that ends the operation: a LOOMORA_ERR_* code (loomora_project.h), a message and
// optionally the Issue it corresponds to.
class FormatError : public std::exception {
  public:
    FormatError(int code, const std::string &message) : code_(code), message_(message), has_issue_(false) {}
    FormatError(int code, const Issue &issue)
        : code_(code), message_(issue.message), issue_(issue), has_issue_(true) {}
    ~FormatError() noexcept override {}

    int code() const { return code_; }
    bool has_issue() const { return has_issue_; }
    const Issue &issue() const { return issue_; } // meaningful only when has_issue()
    const char *what() const noexcept override { return message_.c_str(); }

  private:
    int code_;
    std::string message_;
    Issue issue_;
    bool has_issue_;
};

// The LOOMORA_ERR_* code of an exception: the code of a FormatError, LOOMORA_ERR_NO_MEMORY for
// std::bad_alloc, LOOMORA_ERR_INTERNAL for anything else.
int code_for_exception(const std::exception &e);

// The same for the exception being handled; call it only inside a catch block (it rethrows to
// classify it, and also maps a non-std exception to LOOMORA_ERR_INTERNAL). Never throws.
int code_of_current_exception() noexcept;

} // namespace format
} // namespace loomora

#endif

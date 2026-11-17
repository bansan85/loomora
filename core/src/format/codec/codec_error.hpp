// Helper shared by the binary codecs (sequence, mesh, image metadata): builds the FormatError that
// carries a stable issue code ("seq.crc", "mesh.index_range", ...) so that the reader and the
// validator can put it into a Report with Report::add(error, code, entry) without any translation.
#ifndef LOOMORA_FORMAT_CODEC_CODEC_ERROR_HPP
#define LOOMORA_FORMAT_CODEC_CODEC_ERROR_HPP

#include <string>

#include "loomora.h"
#include "loomora_project.h"
#include "format/report.hpp"

namespace loomora {
namespace format {
namespace codec {

// An error-severity Issue with `issue_code` and `message` (the entry and the item are left for the
// caller to fill: the codec does not know which archive entry it decodes).
inline Issue make_issue(const char *issue_code, const std::string &message) {
    Issue issue;
    issue.severity = kError;
    issue.code = issue_code;
    issue.message = message;
    return issue;
}

// The data being decoded is damaged: LOOMORA_ERR_CORRUPT carrying the issue.
[[noreturn]] inline void fail_corrupt(const char *issue_code, const std::string &message) {
    throw FormatError(LOOMORA_ERR_CORRUPT, make_issue(issue_code, message));
}

// The data handed to an encoder is not valid: LOOMORA_ERR_INVALID_ITEM carrying the issue.
[[noreturn]] inline void fail_invalid(const char *issue_code, const std::string &message) {
    throw FormatError(LOOMORA_ERR_INVALID_ITEM, make_issue(issue_code, message));
}

// A misuse of a codec object by the caller (a bug): LOOMORA_ERR_INTERNAL.
[[noreturn]] inline void fail_internal(const std::string &message) {
    throw FormatError(LOOMORA_ERR_INTERNAL, message);
}

} // namespace codec
} // namespace format
} // namespace loomora

#endif

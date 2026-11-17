#include "format/model/common.hpp"

#include <cstring>

namespace loomora {
namespace format {
namespace model {

namespace {

bool is_hex_digit(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool is_lower_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }

std::string decimal(std::uint64_t v) {
    char buf[24];
    int n = 0;
    do {
        buf[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v != 0);
    std::string s;
    while (n > 0)
        s.push_back(buf[--n]);
    return s;
}

int number_at(const std::string &s, std::size_t pos, std::size_t n) { // n digits, caller checked
    int v = 0;
    for (std::size_t i = 0; i < n; ++i)
        v = v * 10 + (s[pos + i] - '0');
    return v;
}

} // namespace

bool is_hex_color(const std::string &s) {
    if (s.size() != 7 || s[0] != '#')
        return false;
    for (std::size_t i = 1; i < 7; ++i)
        if (!is_hex_digit(s[i]))
            return false;
    return true;
}

bool is_iso8601_utc(const std::string &s) {
    // YYYY-MM-DDTHH:MM:SS, then optional .digits, then Z
    if (s.size() < 20 || s[s.size() - 1] != 'Z')
        return false;
    static const char shape[] = "dddd-dd-ddTdd:dd:dd";
    for (std::size_t i = 0; i < 19; ++i) {
        if (shape[i] == 'd' ? !is_digit(s[i]) : s[i] != shape[i])
            return false;
    }
    if (s.size() > 20) { // fraction: '.' then at least one digit, up to the final Z
        if (s[19] != '.' || s.size() < 22)
            return false;
        for (std::size_t i = 20; i + 1 < s.size(); ++i)
            if (!is_digit(s[i]))
                return false;
    }
    const int month = number_at(s, 5, 2), day = number_at(s, 8, 2);
    const int hour = number_at(s, 11, 2), minute = number_at(s, 14, 2), second = number_at(s, 17, 2);
    return month >= 1 && month <= 12 && day >= 1 && day <= 31 && hour <= 23 && minute <= 59 && second <= 60;
}

bool is_uuid4(const std::string &s) {
    if (s.size() != 36)
        return false;
    for (std::size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-')
                return false;
        } else if (!is_lower_hex(s[i])) {
            return false;
        }
    }
    return s[14] == '4' && (s[19] == '8' || s[19] == '9' || s[19] == 'a' || s[19] == 'b');
}

bool is_sha256_hex(const std::string &s) {
    if (s.size() != 64)
        return false;
    for (std::size_t i = 0; i < 64; ++i)
        if (!is_lower_hex(s[i]))
            return false;
    return true;
}

void add_error(Report &report, const char *code, const std::string &entry, ItemKind kind, std::uint32_t id,
               const std::string &message) {
    report.add(kError, code, entry, message, kind, id);
}

void add_value_error(Report &report, const std::string &entry, ItemKind kind, std::uint32_t id,
                     const std::string &message) {
    add_error(report, "schema.value", entry, kind, id, message);
}

bool check_finite(Report &report, const std::string &entry, ItemKind kind, std::uint32_t id, const char *what,
                  double value) {
    if (is_finite(value))
        return true;
    add_error(report, "num.not_finite", entry, kind, id, std::string(what) + " is not a finite number");
    return false;
}

bool check_positive(Report &report, const std::string &entry, ItemKind kind, std::uint32_t id, const char *what,
                    double value) {
    if (!check_finite(report, entry, kind, id, what, value))
        return false;
    if (value > 0)
        return true;
    add_value_error(report, entry, kind, id, std::string(what) + " must be greater than 0");
    return false;
}

void check_length(Report &report, const std::string &entry, ItemKind kind, std::uint32_t id, const char *what,
                  const std::string &value, std::size_t max_bytes) {
    if (value.size() > max_bytes)
        add_value_error(report, entry, kind, id, std::string(what) + " is longer than " + decimal(max_bytes) + " bytes");
}

bool lookup_name(const char *const *names, std::size_t count, const std::string &text, int &index) {
    for (std::size_t i = 0; i < count; ++i) {
        if (text == names[i]) {
            index = static_cast<int>(i);
            return true;
        }
    }
    return false;
}

void check_entry_ref(Report &report, const std::string &entry, ItemKind kind, std::uint32_t id, const char *what,
                     const EntryRef &ref) {
    const std::string w(what);
    if (ref.entry.size() > kMaxEntryBytes)
        add_value_error(report, entry, kind, id, w + ".entry is longer than 512 bytes");
    if (!ref.format.empty() && ref.format != "png" && ref.format != "jpeg" && ref.format != "webp")
        add_value_error(report, entry, kind, id, w + ".format must be png, jpeg or webp");
    if (ref.size < -1)
        add_value_error(report, entry, kind, id, w + ".size must not be negative");
}

} // namespace model
} // namespace format
} // namespace loomora

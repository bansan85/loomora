// JNI glue of the Android UIs (Legacy: Java; Modern: Kotlin, same class): the functions of
// contracts/c-api.md, called as fr.bansan.loomora.probe.Native.<name>.
//
// The glue only converts between Java strings and UTF-8 and forwards the call: no logic. A Java
// string is UTF-16 and JNI's GetStringUTFChars gives "modified UTF-8" (a character outside the
// BMP becomes two 3-byte surrogates), which the backend rejects as invalid UTF-8, so the
// conversion is done here with GetStringChars / NewString. An unpaired surrogate (it cannot be
// written as UTF-8) is converted to U+FFFD. No exception crosses the JNI boundary: every
// function catches everything and returns a null / error value.
//
// C++11, built into libloomora_jni.so together with the static backend (no other dependency
// than the C and C++ runtime linked statically, liblog is not used).
#include <jni.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "loomora.h"

namespace {

// Appends the UTF-8 form of the UTF-16 text to `out`.
void Utf16ToUtf8(const jchar *s, jsize n, std::string &out) {
    for (jsize i = 0; i < n; i++) {
        std::uint32_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (static_cast<std::uint32_t>(s[i + 1]) - 0xDC00);
            i++;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD; // unpaired surrogate
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
}

// Reads a Java string as UTF-8. A null string gives false.
bool ToUtf8(JNIEnv *env, jstring s, std::string &out) {
    out.clear();
    if (s == NULL)
        return false;
    const jsize n = env->GetStringLength(s);
    const jchar *chars = env->GetStringChars(s, NULL);
    if (chars == NULL)
        return false;
    try {
        out.reserve(static_cast<std::size_t>(n) * 3);
        Utf16ToUtf8(chars, n, out);
    } catch (...) {
        env->ReleaseStringChars(s, chars);
        return false;
    }
    env->ReleaseStringChars(s, chars);
    return true;
}

// Makes a Java string from `n` bytes of UTF-8 (a malformed sequence gives U+FFFD).
jstring FromUtf8(JNIEnv *env, const char *bytes, std::size_t n) {
    std::vector<jchar> chars;
    try {
        chars.reserve(n);
        std::size_t i = 0;
        while (i < n) {
            const std::uint32_t b = static_cast<unsigned char>(bytes[i]);
            std::uint32_t c = 0xFFFD;
            std::size_t len = 1;
            if (b < 0x80) {
                c = b;
            } else if (b >= 0xC2 && b <= 0xDF && i + 1 < n) {
                const std::uint32_t b1 = static_cast<unsigned char>(bytes[i + 1]);
                if ((b1 & 0xC0) == 0x80) {
                    c = ((b & 0x1F) << 6) | (b1 & 0x3F);
                    len = 2;
                }
            } else if (b >= 0xE0 && b <= 0xEF && i + 2 < n) {
                const std::uint32_t b1 = static_cast<unsigned char>(bytes[i + 1]);
                const std::uint32_t b2 = static_cast<unsigned char>(bytes[i + 2]);
                if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80) {
                    c = ((b & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
                    len = 3;
                }
            } else if (b >= 0xF0 && b <= 0xF4 && i + 3 < n) {
                const std::uint32_t b1 = static_cast<unsigned char>(bytes[i + 1]);
                const std::uint32_t b2 = static_cast<unsigned char>(bytes[i + 2]);
                const std::uint32_t b3 = static_cast<unsigned char>(bytes[i + 3]);
                if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80 && (b3 & 0xC0) == 0x80) {
                    c = ((b & 0x07) << 18) | ((b1 & 0x3F) << 12) | ((b2 & 0x3F) << 6) |
                        (b3 & 0x3F);
                    len = 4;
                }
            }
            i += len;
            if (c >= 0x10000) {
                c -= 0x10000;
                chars.push_back(static_cast<jchar>(0xD800 + (c >> 10)));
                chars.push_back(static_cast<jchar>(0xDC00 + (c & 0x3FF)));
            } else {
                chars.push_back(static_cast<jchar>(c));
            }
        }
    } catch (...) {
        return NULL;
    }
    return env->NewString(chars.empty() ? NULL : &chars[0], static_cast<jsize>(chars.size()));
}

} // namespace

extern "C" {

JNIEXPORT jint JNICALL Java_fr_bansan_loomora_probe_Native_abiVersion(JNIEnv *, jclass) {
    return static_cast<jint>(loomora_abi_version());
}

// The greeting of `input`, or null when the backend refuses it (error code < 0).
JNIEXPORT jstring JNICALL Java_fr_bansan_loomora_probe_Native_greet(JNIEnv *env, jclass, jstring input) {
    std::string in;
    if (!ToUtf8(env, input, in))
        return NULL;
    try {
        std::vector<std::uint8_t> out(in.size() + 64);
        const std::int32_t n = loomora_greet(in.data(), in.size(), &out[0], out.size());
        if (n < 0)
            return NULL;
        return FromUtf8(env, reinterpret_cast<const char *>(&out[0]),
                        static_cast<std::size_t>(n));
    } catch (...) {
        return NULL;
    }
}

JNIEXPORT jint JNICALL Java_fr_bansan_loomora_probe_Native_isEol(JNIEnv *, jclass, jint family,
                                                           jint major, jint minor) {
    return static_cast<jint>(loomora_is_eol(family, major, minor));
}

JNIEXPORT jstring JNICALL Java_fr_bansan_loomora_probe_Native_eolWarning(JNIEnv *env, jclass) {
    const char *text = loomora_eol_warning();
    return FromUtf8(env, text, std::char_traits<char>::length(text));
}

JNIEXPORT jstring JNICALL Java_fr_bansan_loomora_probe_Native_buildCompiler(JNIEnv *env, jclass) {
    const char *text = loomora_build_compiler();
    return FromUtf8(env, text, std::char_traits<char>::length(text));
}

// The probe JSON line (ASCII), or null when the backend could not build it.
JNIEXPORT jstring JNICALL Java_fr_bansan_loomora_probe_Native_probeRecord(
    JNIEnv *env, jclass, jstring platformId, jint uiVariant, jint coreKind, jstring input,
    jstring displayedText, jint displayed, jint displayMs, jlong rssKb, jint eolWarningShown,
    jstring disabledFeatures, jstring timestampUtc) {
    std::string platform, in, shown, disabled, timestamp;
    if (!ToUtf8(env, platformId, platform) || !ToUtf8(env, input, in) ||
        !ToUtf8(env, displayedText, shown) || !ToUtf8(env, disabledFeatures, disabled) ||
        !ToUtf8(env, timestampUtc, timestamp))
        return NULL;
    try {
        const std::size_t needed = loomora_probe_record(
            platform.c_str(), uiVariant, coreKind, in.c_str(), shown.c_str(), displayed,
            static_cast<std::uint32_t>(displayMs), static_cast<std::int64_t>(rssKb),
            eolWarningShown, disabled.c_str(), timestamp.c_str(), NULL, 0);
        if (needed == 0)
            return NULL;
        std::vector<std::uint8_t> buf(needed);
        const std::size_t written = loomora_probe_record(
            platform.c_str(), uiVariant, coreKind, in.c_str(), shown.c_str(), displayed,
            static_cast<std::uint32_t>(displayMs), static_cast<std::int64_t>(rssKb),
            eolWarningShown, disabled.c_str(), timestamp.c_str(), &buf[0], needed);
        if (written != needed)
            return NULL;
        return FromUtf8(env, reinterpret_cast<const char *>(&buf[0]), written);
    } catch (...) {
        return NULL;
    }
}

} // extern "C"

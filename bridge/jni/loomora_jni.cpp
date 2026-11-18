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
// The project functions (contracts/c-api-project.md) are only the ones the probe modes of spec 002
// (contracts/ui-validate-project.md) need: open, validate, export to a byte array, read of an item
// and of an entry as byte arrays, report counts and the closing calls. A handle (project, report)
// travels as a jlong that the Java side never interprets.
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

// A project or report handle as a jlong and back.
template <class T> jlong ToHandle(T *pointer) {
    return static_cast<jlong>(reinterpret_cast<std::intptr_t>(pointer));
}

template <class T> T *FromHandle(jlong handle) {
    return reinterpret_cast<T *>(static_cast<std::intptr_t>(handle));
}

// A Java byte[] holding `n` bytes; null when it cannot be made (too long for an array, no memory).
jbyteArray ToByteArray(JNIEnv *env, const std::uint8_t *data, std::size_t n) {
    if (n > static_cast<std::size_t>(0x7FFFFFFF))
        return NULL;
    jbyteArray array = env->NewByteArray(static_cast<jsize>(n));
    if (array == NULL)
        return NULL;
    if (n > 0)
        env->SetByteArrayRegion(array, 0, static_cast<jsize>(n),
                                reinterpret_cast<const jbyte *>(data));
    return array;
}

// { code, project handle, report handle } as a long[3]; null when it cannot be made.
jlongArray ToTriple(JNIEnv *env, jlong code, jlong project, jlong report) {
    jlongArray array = env->NewLongArray(3);
    if (array == NULL)
        return NULL;
    const jlong values[3] = {code, project, report};
    env->SetLongArrayRegion(array, 0, 3, values);
    return array;
}

// The sink of loomora_project_export_stream: appends each chunk to a growing memory buffer.
struct Sink {
    std::vector<std::uint8_t> bytes;
    bool failed;
};

std::int32_t AppendChunk(void *user, const std::uint8_t *data, std::size_t length) {
    Sink *sink = static_cast<Sink *>(user);
    try {
        sink->bytes.insert(sink->bytes.end(), data, data + length);
    } catch (...) {
        sink->failed = true;
        return 1; // stops the export
    }
    return 0;
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

// ---- project reader and export (spec 002) ----

// Opens a project file (loomora_project_open_path): { code, project handle, report handle }. The
// project handle is 0 on failure, the report handle is 0 when there is no report. Null only when the
// path is null or the glue itself failed.
JNIEXPORT jlongArray JNICALL Java_fr_bansan_loomora_probe_Native_projectOpen(JNIEnv *env, jclass,
                                                                            jstring path) {
    std::string utf8;
    if (!ToUtf8(env, path, utf8))
        return NULL;
    loomora_project *project = NULL;
    loomora_report *report = NULL;
    try {
        const std::int32_t rc = loomora_project_open_path(utf8.c_str(), NULL, NULL, &project, &report);
        jlongArray result = ToTriple(env, rc, ToHandle(project), ToHandle(report));
        if (result == NULL) {
            if (project != NULL)
                loomora_project_close(project);
            if (report != NULL)
                loomora_report_free(report);
        }
        return result;
    } catch (...) {
        return NULL;
    }
}

// Validates a project held in a byte array (loomora_validate_memory): { code, 0, report handle }.
// Null when the glue failed.
JNIEXPORT jlongArray JNICALL Java_fr_bansan_loomora_probe_Native_validateMemory(JNIEnv *env, jclass,
                                                                               jbyteArray data) {
    if (data == NULL)
        return NULL;
    const jsize n = env->GetArrayLength(data);
    jbyte *bytes = env->GetByteArrayElements(data, NULL);
    if (bytes == NULL)
        return NULL;
    loomora_report *report = NULL;
    std::int32_t rc = 0;
    try {
        rc = loomora_validate_memory(reinterpret_cast<const std::uint8_t *>(bytes),
                                     static_cast<std::size_t>(n), NULL, NULL, &report);
    } catch (...) {
        env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);
        return NULL;
    }
    env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);
    jlongArray result = ToTriple(env, rc, 0, ToHandle(report));
    if (result == NULL && report != NULL)
        loomora_report_free(report);
    return result;
}

JNIEXPORT jlong JNICALL Java_fr_bansan_loomora_probe_Native_reportCount(JNIEnv *, jclass,
                                                                       jlong report) {
    return report == 0 ? 0
                       : static_cast<jlong>(loomora_report_count(FromHandle<loomora_report>(report)));
}

JNIEXPORT jlong JNICALL Java_fr_bansan_loomora_probe_Native_reportErrorCount(JNIEnv *, jclass,
                                                                            jlong report) {
    return report == 0
               ? 0
               : static_cast<jlong>(loomora_report_error_count(FromHandle<loomora_report>(report)));
}

JNIEXPORT void JNICALL Java_fr_bansan_loomora_probe_Native_reportFree(JNIEnv *, jclass, jlong report) {
    if (report != 0)
        loomora_report_free(FromHandle<loomora_report>(report));
}

JNIEXPORT void JNICALL Java_fr_bansan_loomora_probe_Native_projectClose(JNIEnv *, jclass,
                                                                       jlong project) {
    if (project != 0)
        loomora_project_close(FromHandle<loomora_project>(project));
}

// 1 for a light project, 0 for a full one (loomora_project_is_light).
JNIEXPORT jint JNICALL Java_fr_bansan_loomora_probe_Native_projectIsLight(JNIEnv *, jclass,
                                                                         jlong project) {
    return project == 0
               ? 0
               : static_cast<jint>(loomora_project_is_light(FromHandle<loomora_project>(project)));
}

// A copy of every result written into memory (loomora_project_export_stream, no result subset,
// options 0): the archive, or null with rc[0] set to the error code (0 on success). `profile` is
// LOOMORA_PROFILE_FULL or LOOMORA_PROFILE_LIGHT, `modifiedAt` the caller's time (ISO 8601 UTC).
JNIEXPORT jbyteArray JNICALL Java_fr_bansan_loomora_probe_Native_projectExport(
    JNIEnv *env, jclass, jlong project, jint profile, jstring modifiedAt, jintArray rc) {
    std::string stamp;
    jint code = LOOMORA_ERR_INVALID;
    jbyteArray result = NULL;
    if (project != 0 && ToUtf8(env, modifiedAt, stamp)) {
        Sink sink;
        sink.failed = false;
        loomora_report *report = NULL;
        try {
            code = loomora_project_export_stream(FromHandle<loomora_project>(project), profile, NULL,
                                                 0, stamp.c_str(), 0, AppendChunk, &sink, &report);
            if (report != NULL)
                loomora_report_free(report);
            if (code == 0 && sink.failed)
                code = LOOMORA_ERR_NO_MEMORY;
            if (code == 0) {
                result = ToByteArray(env, sink.bytes.empty() ? NULL : &sink.bytes[0],
                                     sink.bytes.size());
                if (result == NULL)
                    code = LOOMORA_ERR_NO_MEMORY;
            }
        } catch (...) {
            code = LOOMORA_ERR_NO_MEMORY;
            result = NULL;
        }
    }
    if (rc != NULL && env->GetArrayLength(rc) > 0)
        env->SetIntArrayRegion(rc, 0, 1, &code);
    return result;
}

// The JSON of one item (kind: LOOMORA_KIND_*), or null (unknown item, no memory).
JNIEXPORT jbyteArray JNICALL Java_fr_bansan_loomora_probe_Native_projectItemJson(
    JNIEnv *env, jclass, jlong project, jint kind, jint id) {
    if (project == 0)
        return NULL;
    const loomora_project *p = FromHandle<loomora_project>(project);
    try {
        const std::int64_t need =
            loomora_project_item_json(p, kind, static_cast<std::uint32_t>(id), NULL, 0);
        if (need < 0)
            return NULL;
        std::vector<std::uint8_t> buf(static_cast<std::size_t>(need) + 1);
        const std::int64_t got = loomora_project_item_json(p, kind, static_cast<std::uint32_t>(id),
                                                           &buf[0], static_cast<std::size_t>(need));
        if (got != need)
            return NULL;
        return ToByteArray(env, &buf[0], static_cast<std::size_t>(need));
    } catch (...) {
        return NULL;
    }
}

// The whole content of an entry (for example "manifest.json"), or null (unknown entry, no memory,
// an entry too long for a byte array).
JNIEXPORT jbyteArray JNICALL Java_fr_bansan_loomora_probe_Native_projectReadEntry(
    JNIEnv *env, jclass, jlong project, jstring entry) {
    std::string name;
    if (project == 0 || !ToUtf8(env, entry, name))
        return NULL;
    const loomora_project *p = FromHandle<loomora_project>(project);
    try {
        const std::int64_t size = loomora_project_entry_size(p, name.c_str());
        if (size < 0 || size > 0x7FFFFFFF)
            return NULL;
        std::vector<std::uint8_t> buf(static_cast<std::size_t>(size) + 1);
        const std::int64_t got =
            loomora_project_read_entry(p, name.c_str(), 0, &buf[0], static_cast<std::size_t>(size));
        if (got != size)
            return NULL;
        return ToByteArray(env, &buf[0], static_cast<std::size_t>(size));
    } catch (...) {
        return NULL;
    }
}

} // extern "C"

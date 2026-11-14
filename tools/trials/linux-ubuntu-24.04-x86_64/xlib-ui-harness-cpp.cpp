// Harness for the C++ Linux Legacy (Xlib) UI, adapted from xlib-ui-harness.c (which is left as it
// was: it drives the earlier C version of the UI). It includes main.cpp with its main() renamed,
// opens the window and the text engine through App::Init the way the program does, then feeds hard
// texts straight to the drawing code (Redraw: MakeBlock, WrapBlock, DrawGlyphs), including invalid
// UTF-8 that the greeting backend and --probe-input would refuse, so that a sanitizer build can
// check the memory behaviour. The private members of App are made reachable with a macro: this is
// a test harness, not product code.
//
// Build (from ui/legacy/linux-xlib; <core objects> are core/src/*.cpp compiled with the same flags):
//   g++ -g -fsanitize=address,undefined -fno-omit-frame-pointer -std=c++11 -Dmain=loomora_ui_main
//       -I../../../core/include -I. ../../../tools/trials/linux-ubuntu-24.04-x86_64/xlib-ui-harness-cpp.cpp
//       probe.cpp xftdl.cpp keys.cpp <core objects> -lX11 -ldl
// Run with $DISPLAY set; LOOMORA_XLIB_NO_XFT=1 exercises the core font path, LOOMORA_XLIB_NO_XIM=1
// the key path without an input method. Prints the engine in use and one line per text.
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/utsname.h>

#include <cerrno>
#include <clocale>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#define private public
#include "../../../ui/legacy/linux-xlib/main.cpp"
#undef private

#undef main

namespace {

const std::size_t kMaxBytes = 4 * kMaxInputChars;

void RunText(App &ui, const char *name, const unsigned char *bytes, std::size_t len, int how) {
    timeval t0;
    timeval t1;
    gettimeofday(&t0, nullptr);
    if (len > kMaxBytes)
        len = kMaxBytes;
    ui.input_.assign(reinterpret_cast<const char *>(bytes), len);
    ui.inputChars_ = 0;
    for (std::size_t i = 0; i < len; i++)
        if ((static_cast<unsigned char>(ui.input_[i]) & 0xC0) != 0x80)
            ui.inputChars_++;
    if (how == 0) {
        ui.UpdateGreeting(); // the backend refuses invalid UTF-8: an error message is shown
    } else {
        // straight to the drawing code: the greeting holds the same raw bytes
        ui.greeting_.assign(reinterpret_cast<const char *>(bytes), len);
        const Block g = ui.MakeBlock(ui.greeting_, kNoWrap, true);
        ui.substituted_ = g.substituted > 0;
        ui.missing_ = g.missing > 0;
        ui.MakeBlock(ui.input_, kNoWrap, false);
    }
    ui.Redraw();
    XSync(ui.display_.get(), False);
    gettimeofday(&t1, nullptr);
    std::printf("%-44s bytes %4lu chars %4lu substituted %d missing %d  %ld ms\n", name,
                static_cast<unsigned long>(len), static_cast<unsigned long>(ui.inputChars_),
                ui.substituted_ ? 1 : 0, ui.missing_ ? 1 : 0,
                static_cast<long>((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000));
}

#define TEXT(s) reinterpret_cast<const unsigned char *>(s)

int Harness() {
    App ui;
    if (ui.Init("Coucou") != 0)
        return 1;
    ui.eol_ = true; // draws the banner path as well
    XMapWindow(ui.display_.get(), ui.window_.id());
    XSync(ui.display_.get(), False);
    std::printf("engine: %s; input method: %s\n", ui.fonts_ ? "Xft" : "core font",
                ui.input_method_.ic() != nullptr ? "yes (Xutf8LookupString)"
                                                 : "none (XLookupString + keysym table)");

    std::vector<unsigned char> big(kMaxBytes + 8);
    std::vector<unsigned char> rnd(kMaxBytes + 8);
    unsigned int seed = 12345;

    RunText(ui, "empty", TEXT(""), 0, 0);
    RunText(ui, "mixed",
            TEXT("Zo\xc3\xa9 \xe2\x82\xac \xce\x95\xce\xbb \xd7\xa9 \xd9\x85 \xe6\x97\xa5 \xf0\x9f\x98\x80"),
            28, 0);
    RunText(ui, "newline and tab", TEXT("a\nb\tc\r\n"), 7, 0);
    RunText(ui, "NUL-free control chars", TEXT("\x01\x02\x1b\x7f"), 4, 1);
    RunText(ui, "invalid: lone continuation", TEXT("a\x80\x80z"), 4, 1);
    RunText(ui, "invalid: truncated 3-byte", TEXT("ab\xe6\x97"), 4, 1);
    RunText(ui, "invalid: overlong", TEXT("\xc0\x80\xe0\x80\x80\xf0\x80\x80\x80"), 9, 1);
    RunText(ui, "invalid: surrogate D800", TEXT("\xed\xa0\x80"), 3, 1);
    RunText(ui, "invalid: above 10FFFF", TEXT("\xf4\x90\x80\x80\xf8\x88\x80\x80\x80"), 9, 1);
    RunText(ui, "invalid: 0xFF 0xFE", TEXT("\xff\xfe"), 2, 1);
    RunText(ui, "invalid through the backend", TEXT("\xff\xfe\xe6"), 3, 0);
    RunText(ui, "noncharacters + private use",
            TEXT("\xef\xbf\xbe\xef\xbf\xbf\xee\x80\x80\xf3\xb0\x80\x80"), 13, 0);
    RunText(ui, "format characters", TEXT("a\xe2\x80\x8d\xe2\x80\x8b\xef\xb8\x8f\xe2\x80\xae" "b"), 15,
            0);
    RunText(ui, "combining + ZWJ family",
            TEXT("e\xcc\x81 \xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7"),
            22, 0);
    for (std::size_t i = 0; i < 1000; i++) {
        big[i * 2] = 0xD0; // U+0436
        big[i * 2 + 1] = 0xB6;
    }
    RunText(ui, "1000 two-byte (2000 bytes)", &big[0], 2000, 0);
    for (std::size_t i = 0; i < 1000; i++) {
        big[i * 3] = 0xE6; // U+65E5
        big[i * 3 + 1] = 0x97;
        big[i * 3 + 2] = 0xA5;
    }
    RunText(ui, "1000 three-byte (3000 bytes)", &big[0], 3000, 0);
    for (std::size_t i = 0; i < 1000; i++) {
        big[i * 4] = 0xF0; // U+1F600
        big[i * 4 + 1] = 0x9F;
        big[i * 4 + 2] = 0x98;
        big[i * 4 + 3] = 0x80;
    }
    RunText(ui, "1000 four-byte (4000 bytes)", &big[0], 4000, 0);
    std::memset(&big[0], 'x', 1000);
    RunText(ui, "1000 ASCII, one word", &big[0], 1000, 0);
    // random bytes, valid or not, drawn raw
    for (std::size_t i = 0; i < rnd.size(); i++) {
        seed = seed * 1103515245u + 12345u;
        rnd[i] = static_cast<unsigned char>(seed >> 16);
    }
    RunText(ui, "4000 random bytes, drawn raw", &rnd[0], 4000, 1);
    for (std::size_t i = 0; i < rnd.size(); i++) {
        seed = seed * 1103515245u + 12345u;
        rnd[i] = static_cast<unsigned char>(0x80 | ((seed >> 16) & 0x7F));
    }
    RunText(ui, "4000 continuation bytes, drawn raw", &rnd[0], 4000, 1);
    // narrow windows: wrapping inside a word, a window narrower than a character
    for (int w = 1; w <= 40; w += 13) {
        ui.width_ = w;
        ui.height_ = 20;
        RunText(ui, "narrow window", TEXT("Zo\xc3\xa9 \xe6\x97\xa5\xe6\x97\xa5 long words wrap here"), 31, 0);
    }
    ui.width_ = kStartWidth;
    ui.height_ = kStartHeight;
    // the append and delete paths: 1,010 characters typed, the limit holds
    ui.input_.clear();
    ui.inputChars_ = 0;
    for (std::size_t i = 0; i < 1010; i++)
        ui.AppendChar(i % 3 == 0 ? 0x65E5 : (i % 3 == 1 ? 0x436 : 0x1F600));
    std::printf("typed 1010 characters: %lu characters, %lu bytes (limit %lu characters)\n",
                static_cast<unsigned long>(ui.inputChars_), static_cast<unsigned long>(ui.input_.size()),
                static_cast<unsigned long>(kMaxInputChars));
    for (std::size_t i = 0; i < 1005; i++)
        ui.DeleteLast();
    std::printf("after 1005 deletions: %lu characters, %lu bytes, valid UTF-8 %d\n",
                static_cast<unsigned long>(ui.inputChars_), static_cast<unsigned long>(ui.input_.size()),
                Utf8Valid(ui.input_.c_str()) ? 1 : 0);
    return 0;
}

} // namespace

int main() {
    try {
        return Harness();
    } catch (const std::exception &e) {
        std::fprintf(stderr, "harness: %s\n", e.what());
        return 1;
    }
}

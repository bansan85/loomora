// Main window of the Linux Modern UI: see main_window.hpp.
#include "main_window.hpp"

#include <sys/utsname.h>

#include <array>
#include <cstdint>
#include <exception>
#include <sstream>
#include <string>
#include <utility>

#include "loomora.h"

namespace loomora {

namespace {

constexpr int kMaxInputChars = 1000;
// Largest greeting: 1,000 characters of at most 4 bytes each, plus "Bonjour ".
constexpr std::size_t kGreetingCap = 4096 + 64;

// Major and minor kernel version, the version the backend's Linux end-of-life rule is expressed
// in. Returns false when it cannot be read.
bool kernel_version(int& major, int& minor) {
    struct utsname un;
    if (uname(&un) != 0)
        return false;
    // Not sscanf: with glibc 2.38 and GCC 13 it binds to __isoc23_sscanf (GLIBC_2.38), which
    // would raise the glibc version the executable asks for for no reason.
    std::istringstream release(un.release);
    char dot = '\0';
    return static_cast<bool>(release >> major >> dot >> minor) && dot == '.';
}

// Horizontal alignment of the text of a wrapped label: left, mirrored in a right-to-left
// locale. Gtk::Misc alignment is the 3.10 way.
void set_label_text_start(Gtk::Label& label) {
#ifdef LOOMORA_GTK_HAS_LABEL_XALIGN
    label.set_xalign(0.0f);
#else
    label.set_alignment(0.0f, 0.5f);
#endif
}

// The text of the greeting: centred, 1.8 times the size of the theme font, wrapped on words and
// on characters for text without spaces.
void style_greeting_label(Gtk::Label& greeting) {
#ifdef LOOMORA_GTK4
    greeting.set_wrap(true);
    greeting.set_wrap_mode(compat::kWrapWordChar);
#else
    greeting.set_line_wrap(true);
    greeting.set_line_wrap_mode(compat::kWrapWordChar);
#endif
    greeting.set_justify(compat::kJustifyCenter);
    greeting.set_halign(compat::kAlignCenter);
    greeting.set_valign(compat::kAlignCenter);
    Pango::AttrList attrs;  // colours come from the theme
    Pango::AttrFloat scale = Pango::Attribute::create_attr_scale(1.8);
    attrs.insert(scale);
    greeting.set_attributes(attrs);
}

}  // namespace

#ifdef LOOMORA_GTK4

EolBar::EolBar() : Gtk::Box(compat::kHorizontal, 8), text_(loomora_eol_warning()) {
    icon_.set_from_icon_name("dialog-warning-symbolic");
    close_.set_icon_name("window-close-symbolic");
    close_.set_has_frame(false);
    text_.set_wrap(true);
    set_label_text_start(text_);
    text_.set_hexpand(true);
    icon_.set_valign(compat::kAlignStart);
    close_.set_valign(compat::kAlignStart);
    close_.signal_clicked().connect([this] { set_visible(false); });
    append(icon_);
    append(text_);
    append(close_);
}

#else

EolBar::EolBar() : text_(loomora_eol_warning()) {
    set_message_type(Gtk::MESSAGE_WARNING);
    text_.set_line_wrap(true);
    set_label_text_start(text_);
    dynamic_cast<Gtk::Container&>(*get_content_area()).add(text_);
    set_show_close_button(true);
    signal_response().connect([this](int) { hide(); });
}

#endif

MainWindow::MainWindow(const std::string& initial_text, std::function<void()> on_displayed)
    : on_displayed_(std::move(on_displayed)), box_(compat::kVertical, 12) {
    set_title("Loomora");
    set_default_size(480, 240);

    // The 12 pixels around the content: a border width in GTK 3, margins of the box in GTK 4
    // (which has no border width).
#ifdef LOOMORA_GTK4
    box_.set_margin_top(12);
    box_.set_margin_bottom(12);
    box_.set_margin_start(12);
    box_.set_margin_end(12);
    set_child(box_);
    // GTK 4 has no "draw" signal: the first display is the end of the first frame the frame
    // clock of the window paints (the handler runs after GTK's own paint handlers).
    signal_realize().connect([this] {
        Glib::RefPtr<Gdk::FrameClock> clock = get_frame_clock();
        if (clock)
            clock->signal_paint().connect([this] { on_displayed_(); }, true);
    });
#else
    set_border_width(12);
    add(box_);
#endif

    int kernel_major = 0;
    int kernel_minor = 0;
    if (kernel_version(kernel_major, kernel_minor) &&
        loomora_is_eol(LOOMORA_FAMILY_LINUX, kernel_major, kernel_minor) == 1) {
        eol_bar_.reset(new EolBar);
#ifdef LOOMORA_GTK4
        box_.append(*eol_bar_);
#else
        box_.pack_start(*eol_bar_, false, false, 0);
#endif
    }

    entry_.set_max_length(kMaxInputChars);  // counts characters, not bytes
    entry_.set_text(initial_text);
#ifdef LOOMORA_GTK4
    box_.append(entry_);
#else
    box_.pack_start(entry_, false, false, 0);
#endif

    // A greeting of up to 1,000 characters wraps to many lines: without a scrolled window the
    // label overflows the window, is clipped (blank at 250 characters) and the window grows,
    // instead of scrolling.
    style_greeting_label(greeting_);
    scroller_.set_policy(compat::kPolicyNever, compat::kPolicyAutomatic);
#ifdef LOOMORA_GTK_HAS_OVERLAY_SCROLLING
    scroller_.set_overlay_scrolling(false);
#endif
#ifdef LOOMORA_GTK4
    scroller_.set_child(greeting_);
    scroller_.set_vexpand(true);
    box_.append(scroller_);
#else
    scroller_.add(greeting_);
    box_.pack_start(scroller_, true, true, 0);
#endif

    entry_.signal_changed().connect([this] { show_greeting(); });
    show_greeting();
}

void MainWindow::show_window() {
#ifdef LOOMORA_GTK4
    present();
#else
    show_all();
    // The GTK 3 info bar is not shown by show_all() of the window: it is shown by the program.
    if (eol_bar_)
        eol_bar_->show_all();
#endif
}

#ifndef LOOMORA_GTK4
bool MainWindow::on_draw(const Cairo::RefPtr<Cairo::Context>& cr) {
    const bool handled = Gtk::Window::on_draw(cr);
    on_displayed_();  // only the first call counts
    return handled;   // let GTK carry on
}
#endif

void MainWindow::show_greeting() {
    // A slot called from GTK's C code: no exception may leave it.
    try {
        const Glib::ustring text = entry_.get_text();
        std::array<std::uint8_t, kGreetingCap> out;
        const std::int32_t n = loomora_greet(text.c_str(), text.bytes(), out.data(), out.size());
        if (n >= 0) {
            // Built from a std::string: the count of Glib::ustring(const char*, n) is in
            // characters, not in bytes.
            greeting_.set_text(std::string(reinterpret_cast<const char*>(out.data()),
                                           static_cast<std::size_t>(n)));
        } else {
            greeting_.set_text("The greeting could not be computed (error " + Glib::ustring::format(n) +
                               ").");
        }
    } catch (const std::exception&) {
        greeting_.set_text("The greeting could not be computed (error -4).");
    }
}

}  // namespace loomora

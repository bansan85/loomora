// Main window of the Linux Modern UI: the input field, the greeting in a scrolled label and the
// end-of-life banner. No application logic: the greeting, the end-of-life test and its warning
// text come from the backend through loomora.h.
#ifndef LOOMORA_GTK3_MAIN_WINDOW_HPP
#define LOOMORA_GTK3_MAIN_WINDOW_HPP

#include <functional>
#include <memory>
#include <string>

#include "gtk_compat.hpp"

namespace loomora {

// The banner shown above the field when the running system is end of life. GTK 3 has GtkInfoBar;
// GTK 4 deprecates it without a replacement, so there it is a plain box with the warning icon,
// the text and a close button.
#ifdef LOOMORA_GTK4
class EolBar : public Gtk::Box {
public:
    EolBar();

private:
    Gtk::Image icon_;
    Gtk::Label text_;
    Gtk::Button close_;
};
#else
class EolBar : public Gtk::InfoBar {
public:
    EolBar();

private:
    Gtk::Label text_;
};
#endif

class MainWindow : public Gtk::Window {
public:
    // `initial_text` fills the field; `on_displayed` is called for every first-draw notification
    // of the window (the probe only keeps the first one).
    MainWindow(const std::string& initial_text, std::function<void()> on_displayed);

    // Shows the window and everything in it.
    void show_window();

    bool eol_warning_shown() const { return eol_bar_ != nullptr; }
    Glib::ustring input_text() const { return entry_.get_text(); }
    Glib::ustring greeting_text() const { return greeting_.get_text(); }

#ifndef LOOMORA_GTK4
protected:
    // GTK 3 has a "draw" signal: the first display is the end of the first draw of the window.
    bool on_draw(const Cairo::RefPtr<Cairo::Context>& cr) override;
#endif

private:
    void show_greeting();

    std::function<void()> on_displayed_;
    Gtk::Box box_;
    std::unique_ptr<EolBar> eol_bar_;
    Gtk::Entry entry_;
    Gtk::Label greeting_;
    Gtk::ScrolledWindow scroller_;
};

}  // namespace loomora

#endif

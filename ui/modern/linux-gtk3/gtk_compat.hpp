// GTK version policy of the Linux Modern UI (CLAUDE.md,
// docs/implementation/adr/0005-gtk-version-policy.md, 0006-linux-modern-ui-gtkmm-cmake.md).
//
// The floor is the GTK 3.10 API: the UI must always build and run with only the 3.10 API. A
// newer feature is used only behind ONE macro of this file, LOOMORA_GTK_HAS_<FEATURE>, computed
// from the version of the headers being built against; without the macro the code takes the 3.10
// path. Every macro below is declared in docs/implementation/validation/fallbacks.json. Code
// outside this file tests the macros, never a GTK or gtkmm version by itself.
//
// Two CMake cache options of CMakeLists.txt drive this file:
//   LOOMORA_GTK_API=4   build against GTK 4 with gtkmm-4.0 (LOOMORA_GTK4); the default is 3 with
//                       gtkmm-3.0.
//   LOOMORA_GTK_FLOOR=ON  pin GDK_VERSION_MAX_ALLOWED to 3.10: every macro that needs a newer GTK
//                       is then off (the "GDK_VERSION_MAX_ALLOWED >= ..." terms), so the floor
//                       build compiles the 3.10 path only, with -Werror=deprecated-declarations
//                       making any GTK or GDK symbol newer than 3.10 fail the build. gtkmm
//                       methods are not marked by GTK, so for them the floor build proves only
//                       that the 3.10 path is the one compiled; the gtkmm calls of that path are
//                       listed with their version in ADR 0006.
#ifndef LOOMORA_GTK_COMPAT_HPP
#define LOOMORA_GTK_COMPAT_HPP

#include <gtk/gtk.h>
#include <gtkmm.h>

// True when the gtkmm headers are at least major.minor. gtkmm 3.x wraps GTK 3.x with the same
// minor number, so a feature of GTK 3.16 is in the gtkmm API only from gtkmm 3.16.
#define LOOMORA_GTKMM_AT_LEAST(major, minor) \
    (GTKMM_MAJOR_VERSION > (major) || (GTKMM_MAJOR_VERSION == (major) && GTKMM_MINOR_VERSION >= (minor)))

#if GTK_MAJOR_VERSION >= 4

// GTK 4 branch (gtkmm-4.0, C++17): a different window construction (set_child, append). The
// features below all exist in GTK 4.
#define LOOMORA_GTK4 1
#define LOOMORA_GTK_HAS_LABEL_XALIGN 1
#define LOOMORA_GTK_HAS_OVERLAY_SCROLLING 1

#else

// Gtk::Label::set_xalign (GTK and gtkmm 3.16) replaces Gtk::Misc::set_alignment (GtkMisc,
// deprecated in 3.14, removed in GTK 4).
#if GTK_CHECK_VERSION(3, 16, 0) && GDK_VERSION_MAX_ALLOWED >= GDK_VERSION_3_16 && \
    LOOMORA_GTKMM_AT_LEAST(3, 16)
#define LOOMORA_GTK_HAS_LABEL_XALIGN 1
#endif

// Gtk::ScrolledWindow::set_overlay_scrolling (GTK and gtkmm 3.16): GTK 3.10 has no overlay
// scrollbars, its scrollbar always takes room next to the content. From 3.16 the default is an
// overlay that is hidden until the pointer moves, so nothing shows that a long greeting scrolls
// and the scrollbar covers the text once shown; switching overlay scrolling off gives the 3.10
// look.
#if GTK_CHECK_VERSION(3, 16, 0) && GDK_VERSION_MAX_ALLOWED >= GDK_VERSION_3_16 && \
    LOOMORA_GTKMM_AT_LEAST(3, 16)
#define LOOMORA_GTK_HAS_OVERLAY_SCROLLING 1
#endif

#endif

namespace loomora {
namespace compat {

// The same names for the enumerations that gtkmm 3 declares as plain enums (Gtk::ALIGN_CENTER)
// and gtkmm 4 as scoped ones (Gtk::Align::CENTER). The gtkmm 3 spelling is the 3.10 API.
#ifdef LOOMORA_GTK4
constexpr Gtk::Align kAlignCenter = Gtk::Align::CENTER;
constexpr Gtk::Align kAlignStart = Gtk::Align::START;
constexpr Gtk::Justification kJustifyCenter = Gtk::Justification::CENTER;
constexpr Pango::WrapMode kWrapWordChar = Pango::WrapMode::WORD_CHAR;
constexpr Gtk::PolicyType kPolicyNever = Gtk::PolicyType::NEVER;
constexpr Gtk::PolicyType kPolicyAutomatic = Gtk::PolicyType::AUTOMATIC;
constexpr Gtk::Orientation kVertical = Gtk::Orientation::VERTICAL;
constexpr Gtk::Orientation kHorizontal = Gtk::Orientation::HORIZONTAL;
#else
constexpr Gtk::Align kAlignCenter = Gtk::ALIGN_CENTER;
constexpr Gtk::Align kAlignStart = Gtk::ALIGN_START;
constexpr Gtk::Justification kJustifyCenter = Gtk::JUSTIFY_CENTER;
constexpr Pango::WrapMode kWrapWordChar = Pango::WRAP_WORD_CHAR;
constexpr Gtk::PolicyType kPolicyNever = Gtk::POLICY_NEVER;
constexpr Gtk::PolicyType kPolicyAutomatic = Gtk::POLICY_AUTOMATIC;
constexpr Gtk::Orientation kVertical = Gtk::ORIENTATION_VERTICAL;
constexpr Gtk::Orientation kHorizontal = Gtk::ORIENTATION_HORIZONTAL;
#endif

// Flags of the application: several instances may run at once, nothing is registered on the
// session bus (a probe run must leave no process behind), and the command line is the program's
// own (--probe, --probe-input), not GApplication's.
#ifdef LOOMORA_GTK4
const Gio::Application::Flags kApplicationFlags =
    Gio::Application::Flags::NON_UNIQUE | Gio::Application::Flags::HANDLES_COMMAND_LINE;
#else
const Gio::ApplicationFlags kApplicationFlags =
    Gio::APPLICATION_NON_UNIQUE | Gio::APPLICATION_HANDLES_COMMAND_LINE;
#endif

}  // namespace compat
}  // namespace loomora

#endif

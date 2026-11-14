// Loomora Modern UI for Linux: C++ and gtkmm. The floor is the GTK 3.10 API; newer GTK features
// are used only behind the LOOMORA_GTK_HAS_* macros of gtk_compat.hpp, and the GTK 4 branch
// (CMake option LOOMORA_GTK_API=4, gtkmm-4.0) is selected by LOOMORA_GTK4. No application
// logic: the greeting, the end-of-life test and its warning text and the probe line all come
// from the backend through the C API of loomora.h, which is the only way this UI talks to it.
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>

#include "gtk_compat.hpp"
#include "loomora.h"
#include "main_window.hpp"
#include "probe.hpp"

namespace {

constexpr unsigned kProbeExitDelayMs = 2000;

class Application : public Gtk::Application {
public:
    static Glib::RefPtr<Application> create(const loomora::ProbeOptions& options,
                                            loomora::Probe& probe) {
#ifdef LOOMORA_GTK4
        return Glib::make_refptr_for_instance<Application>(new Application(options, probe));
#else
        return Glib::RefPtr<Application>(new Application(options, probe));
#endif
    }

    // Status of the program once run() has returned: 0, or 1 when the probe record failed.
    int exit_status() const { return exit_status_; }

    // Destroys the window while the display is still open (the window outlives run()).
    void release_window() { window_.reset(); }

protected:
    Application(const loomora::ProbeOptions& options, loomora::Probe& probe)
        : Gtk::Application("org.loomora.modern", loomora::compat::kApplicationFlags),
          options_(options),
          probe_(probe) {}

    // The command line was handled by main(): only the activation is left.
    int on_command_line(const Glib::RefPtr<Gio::ApplicationCommandLine>&) override {
        activate();
        return 0;
    }

    void on_activate() override {
        window_.reset(new loomora::MainWindow(options_.has_input ? options_.input : "Coucou",
                                              [this] { probe_.mark_displayed(); }));
        add_window(*window_);
        window_->show_window();
        if (options_.enabled) {
            Glib::signal_timeout().connect_once([this] { finish_probe(); }, kProbeExitDelayMs);
        }
    }

private:
    // After 2 seconds the probe line is written to stdout and the program leaves its main loop.
    void finish_probe() {
        bool written = false;
        try {
            written = probe_.write_record(window_->input_text().raw(), window_->greeting_text().raw(),
                                          window_->eol_warning_shown());
        } catch (const std::exception&) {
            std::fprintf(stderr, "loomora: the backend could not build the probe record\n");
        }
        if (!written)
            exit_status_ = 1;
        quit();
    }

    loomora::ProbeOptions options_;
    loomora::Probe& probe_;
    std::unique_ptr<loomora::MainWindow> window_;
    int exit_status_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    loomora::Probe probe;

    if (loomora_abi_version() != LOOMORA_ABI_VERSION) {
        std::fprintf(stderr, "loomora: backend ABI version %u, expected %d\n",
                     static_cast<unsigned>(loomora_abi_version()), LOOMORA_ABI_VERSION);
        return 1;
    }
    const loomora::ProbeOptions options = loomora::parse_probe_args(argc, argv);
    if (options.bad_usage || (options.has_input && !Glib::ustring(options.input).validate())) {
        std::fprintf(stderr, "usage: %s [--probe [--probe-input <utf8>]]\n", argv[0]);
        return 2;
    }

    try {
        Glib::RefPtr<Application> app = Application::create(options, probe);
        const int status = app->run(argc, argv);
        app->release_window();
        return status != 0 ? status : app->exit_status();
#ifndef LOOMORA_GTK4
    } catch (const Glib::Error& error) {  // not a std::exception in glibmm 2.4 (gtkmm 3)
        std::fprintf(stderr, "loomora: %s\n", error.what().c_str());
#endif
    } catch (const std::exception& error) {
        std::fprintf(stderr, "loomora: %s\n", error.what());
    }
    return 1;
}

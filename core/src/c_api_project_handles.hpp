// Definition of the opaque handle that several C API files share (loomora_project.h only declares
// the typedefs). The other handles (loomora_project, loomora_editor) are defined by the file that
// implements them. Internal header, never installed.
#ifndef LOOMORA_C_API_PROJECT_HANDLES_HPP
#define LOOMORA_C_API_PROJECT_HANDLES_HPP

#include "../include/loomora.h"

#include "format/report.hpp"

// A report returned by value to the caller: the issues of an open, a validation, a save or an export.
struct loomora_report {
    loomora::format::Report report;
};

#endif

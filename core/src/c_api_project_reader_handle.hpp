// Definition of the opaque handle loomora_project, shared by the C API files that read it (the reader
// functions, the export). Internal header, never installed. Kept apart from c_api_project_handles.hpp so
// that a test including that one does not pull in the whole reader.
#ifndef LOOMORA_C_API_PROJECT_READER_HANDLE_HPP
#define LOOMORA_C_API_PROJECT_READER_HANDLE_HPP

#include "../include/loomora.h"

#include <memory>

#include "format/project_reader.hpp"

// An open project: the reader (read.cpp opens and closes it; export.cpp only reads it).
struct loomora_project {
    std::unique_ptr<loomora::format::ProjectReader> reader;
};

#endif

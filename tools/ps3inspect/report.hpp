#pragma once

#include <string>

#include "pspocket/recomp/inventory.hpp"
#include "pspocket/recomp/json_writer.hpp"

namespace pspocket::ps3inspect {

struct ReportOptions {
    bool strings = false;
};

// Human-readable report of one file, ending with its Phase 0 summary block.
std::string render_text(const recomp::FileReport& report, const ReportOptions& options);

// One file as a JSON object (same data as the text report).
void write_json(recomp::JsonWriter& json, const recomp::FileReport& report, const ReportOptions& options);

} // namespace pspocket::ps3inspect

// What prepare_game and a session's SPU libraries share (D-236). See jobs.hpp.

#include "jobs.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>

namespace pspocket::prepare::detail {

namespace fs = std::filesystem;

bool read_bytes(const fs::path& p, std::string& out)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool write_bytes(const fs::path& p, const void* data, size_t size)
{

    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size))) return false;
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    return !ec;
}

uint64_t fnv(std::string_view text)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (const char c : text) h = (h ^ static_cast<unsigned char>(c)) * 0x100000001b3ull;
    return h;
}

std::string headers_define(const fs::path& dir)
{
    std::vector<fs::path> headers;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == ".h") headers.push_back(e.path());
    std::sort(headers.begin(), headers.end());
    std::string all;
    for (const fs::path& p : headers) {
        std::string text;
        read_bytes(p, text);
        all += text;
    }
    return std::format("PSPOCKET_HEADERS_HASH=0x{:016x}ull", fnv(all));
}

codegen::BuildJob job_for(const fs::path& source, bool spu, const std::string& headers,
    const std::vector<std::string>& ppu_args, const std::vector<std::string>& spu_args)
{
    std::string text;
    read_bytes(source, text);
    const std::string name = source.filename().string();
    const codegen::FileKind kind = name.ends_with("_reg.c") || name == "game_spu.c" ? codegen::FileKind::Table
        : spu                                                                       ? codegen::FileKind::SpuCode
                                                                                    : codegen::FileKind::PpuCode;
    const bool cold = kind == codegen::FileKind::SpuCode && name.find("_resume_") != std::string::npos;
    const codegen::CostEstimate e = codegen::estimate_cost(text, kind, cold);
    codegen::BuildJob j;
    j.source = source.string();
    j.include_dirs = {source.parent_path().string()};
    if (!headers.empty()) j.defines = {headers};
    j.extra_args = spu ? spu_args : ppu_args;
    j.cost = e.seconds;
    // The cold twins of translated SPU functions (spu-lift's N_resume_*.c, D-961) at -O1: they run only when the run
    // loop resumes a function after a label, and compile in half the time (GTA V's EDGE job's: 7.6 s for 16.0 at -O2).
    if (cold) j.extra_args.push_back("-O1");
    j.memory_mb = e.memory_mb;
    j.group = static_cast<int>(kind);
    return j;
}

} // namespace pspocket::prepare::detail

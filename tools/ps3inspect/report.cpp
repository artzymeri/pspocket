#include "report.hpp"

#include <format>

#include "pspocket/recomp/bytes.hpp"

namespace pspocket::ps3inspect {

using namespace pspocket::recomp;

namespace {

std::string opt_hex(const std::optional<u32>& v) { return v ? hex32(*v) : "?"; }

std::string name_or_unknown(const ResolvedName& r)
{
    if (r.match == NidMatch::Unknown) return "?";
    if (r.match == NidMatch::OtherModule) return r.name + "  (named via another module)";
    return r.name;
}

std::string library_label(const std::string& name) { return name.empty() ? std::string("(nameless)") : name; }

std::string sdk_version_text(u32 v)
{
    // 0x00360001 -> "3.60" (BCD digits of the SDK release).
    return std::format("{:#010x} ({:x}.{:02x})", v, (v >> 20) & 0xFFF, (v >> 12) & 0xFF);
}

std::string bytes_text(u64 n)
{
    if (n >= (u64{1} << 20)) return std::format("{} bytes ({:.1f} MiB)", n, static_cast<double>(n) / (1 << 20));
    if (n >= (u64{1} << 10)) return std::format("{} bytes ({:.1f} KiB)", n, static_cast<double>(n) / (1 << 10));
    return std::format("{} bytes", n);
}

void line(std::string& out, std::string_view text)
{
    out += text;
    out += '\n';
}

template <class... Args>
void linef(std::string& out, std::format_string<Args...> fmt, Args&&... args)
{
    out += std::format(fmt, std::forward<Args>(args)...);
    out += '\n';
}

void text_self(std::string& out, const SelfInfo& self)
{
    linef(out, "SCE header       version {}, key revision {:#06x}{}, type {}, metadata at {:#x}, header size {:#x}, data size {:#x}",
        self.sce.version, self.sce.key_revision, self.debug ? " (debug)" : "", self.sce.type, self.sce.metadata_offset,
        self.sce.header_size, self.sce.data_size);
    if (self.app) {
        linef(out, "app info         auth id {:#018x}, vendor {:#010x}, self type {} ({}), version {:#018x}", self.app->auth_id,
            self.app->vendor_id, self.app->self_type, self_type_name(self.app->self_type), self.app->version);
    }
    if (self.elf) {
        linef(out, "embedded ELF     class {}, type {:#06x} ({}), machine {} ({}), entry {:#x}, {} program headers, {} section headers",
            self.elf->is64() ? 64 : 32, self.elf->type, elf::type_name(self.elf->type), self.elf->machine,
            elf::machine_name(self.elf->machine), self.elf->entry, self.elf->phnum, self.elf->shnum);
    }
    if (self.version) linef(out, "version info     type {}, present {}, size {:#x}", self.version->subheader_type, self.version->present, self.version->size);
    for (const auto& c : self.control) {
        std::string extra;
        if (c.ctrl_flag1) extra = std::format(", ctrl_flag1 {:#010x}", *c.ctrl_flag1);
        if (c.required_system_version) extra = std::format(", required system version {:#x}", *c.required_system_version);
        if (c.npdrm) {
            extra = std::format(", NPDRM content id '{}', license {} ({}), type {}", c.npdrm->content_id, c.npdrm->license,
                npdrm_license_name(c.npdrm->license), c.npdrm->type);
        }
        linef(out, "control info     type {}, size {:#x}{}", c.type, c.size, extra);
    }
    if (!self.segments.empty()) {
        line(out, "segment info (encrypted data in the SELF):");
        line(out, "    #  offset        size          compression  encryption");
        for (usize i = 0; i < self.segments.size(); i++) {
            const auto& s = self.segments[i];
            linef(out, "  {:>3}  {:#012x}  {:#012x}  {:<11}  {}", i, s.offset, s.size,
                s.compression == 2 ? "zlib" : (s.compression == 1 ? "plain" : std::format("{}", s.compression)), s.encryption);
        }
    }
    if (self.debug) {
        linef(out, "debug SELF: the ELF is stored in plain text at {:#x}{}", self.plaintext_elf_offset.value_or(0),
            " (analyzed below)");
    } else {
        line(out, "ENCRYPTED: the segments can only be read after decryption. Decrypt with RPCS3 (Utilities > Decrypt PS3");
        line(out, "Binaries, or `rpcs3 --decrypt <file>`), then run ps3inspect on the .elf/.prx it writes (docs/phase0.md).");
        if (const NpdrmInfo* n = self.npdrm()) {
            linef(out, "NPDRM licence: {} -> {}", n->license,
                n->license == 3 ? "no RAP needed" : (n->license == 1 || n->license == 2 ? "RPCS3 needs the RAP for this content id" : "unknown"));
        } else {
            line(out, "No NPDRM header: no RAP or KLIC is involved.");
        }
    }
}

void text_segments(std::string& out, const FileReport& r)
{
    linef(out, "segments ({}):", r.segments.size());
    line(out, "    #  type                       flags              offset      vaddr       filesz      memsz       align");
    for (usize i = 0; i < r.segments.size(); i++) {
        const auto& p = r.segments[i];
        linef(out, "  {:>3}  {:<25}  {:<17}  {:#010x}  {:#010x}  {:#010x}  {:#010x}  {:#x}{}", i,
            std::format("{} ({:#x})", elf::program_type_name(p.type), p.type), elf::segment_flags_string(p.flags), p.offset,
            p.vaddr, p.filesz, p.memsz, p.align, p.paddr ? std::format("  paddr {:#x}", p.paddr) : "");
    }
}

void text_sections(std::string& out, const FileReport& r)
{
    if (r.sections.empty()) {
        line(out, "sections: none");
        return;
    }
    usize named = 0;
    for (const auto& s : r.sections) named += !s.name.empty();
    linef(out, "sections ({}, {} named):", r.sections.size(), named);
    line(out, "    #  name                      type          addr        offset      size");
    for (usize i = 0; i < r.sections.size(); i++) {
        const auto& s = r.sections[i];
        linef(out, "  {:>3}  {:<24}  {:<12}  {:#010x}  {:#010x}  {:#010x}{}", i, s.name.empty() ? "-" : s.name,
            elf::section_type_name(s.type), s.addr, s.offset, s.size, s.data_in_file || s.type == elf::kShtNobits ? "" : "  (not in file)");
    }
}

void text_ppu(std::string& out, const PpuModule& m)
{
    if (m.process_param) {
        const auto& p = *m.process_param;
        linef(out, "process param    sdk {}, primary prio {}, primary stack {:#x}, malloc page size {:#x}, ppc_seg {}{}{}",
            sdk_version_text(p.sdk_version), p.primary_prio, p.primary_stacksize, p.malloc_pagesize, p.ppc_seg,
            p.crash_dump_param_addr ? std::format(", crash dump param {:#x}", *p.crash_dump_param_addr) : "",
            p.magic_ok() ? "" : std::format(" (BAD MAGIC {:#010x})", p.magic));
        linef(out, "                 size {:#x}, version {:#010x}, at {:#x}", p.size, p.version, p.vaddr);
    }
    if (m.prx_param) {
        const auto& p = *m.prx_param;
        linef(out, "prx param        libent [{:#010x}, {:#010x}), libstub [{:#010x}, {:#010x}), version {:#x}, ver {:#x}{}",
            p.libent_start, p.libent_end, p.libstub_start, p.libstub_end, p.version, p.ver,
            p.magic_ok() ? "" : std::format(" (BAD MAGIC {:#010x})", p.magic));
    }
    if (m.module_info) {
        const auto& mi = *m.module_info;
        linef(out, "module info      '{}' version {}.{} attributes {:#06x}, toc {:#010x}, exports [{:#010x}, {:#010x}), imports [{:#010x}, {:#010x}) (at {:#x}, via {})",
            mi.name, mi.version_major, mi.version_minor, mi.attributes, mi.toc, mi.exports_start, mi.exports_end,
            mi.imports_start, mi.imports_end, mi.vaddr, mi.located_by);
    }
    if (m.tls) linef(out, "TLS              image {:#010x} ({:#x} bytes), per-thread size {:#x}, align {:#x}", m.tls->vaddr, m.tls->image_size, m.tls->size, m.tls->align);
    if (m.relocations.total != 0) {
        std::string types;
        for (const auto& [type, count] : m.relocations.by_type) types += std::format(" {}:{}", type, count);
        linef(out, "relocations      {} total, {} applied, {} unsupported, {} out of range (type:count{})", m.relocations.total,
            m.relocations.applied, m.relocations.unsupported, m.relocations.out_of_range, types);
    }

    const usize nfunc = m.imported_function_count();
    const usize nvar = m.imported_variable_count();
    linef(out, "imports ({} libraries, {} functions, {} variables):", m.imports.size(), nfunc, nvar);
    for (const auto& lib : m.imports) {
        linef(out, "  {}  version {:#06x}, attributes {:#06x}: {} functions, {} variables{}", library_label(lib.name), lib.version,
            lib.attributes, lib.functions.size(), lib.variables.size(),
            lib.num_tlsvar ? std::format(", {} TLS variables", lib.num_tlsvar) : "");
        for (const auto& f : lib.functions) {
            linef(out, "    {:#010x}  {:<44}  stub {:#010x}  slot {:#010x}{}", f.nid, name_or_unknown(f.resolved), f.stub, f.slot,
                f.refs ? std::format("  refs {}", opt_hex(f.refs)) : "");
        }
        for (const auto& v : lib.variables) {
            linef(out, "    {:#010x}  &{:<43}  ref table {:#010x} ({} refs{})", v.nid, name_or_unknown(v.resolved), v.ref_table,
                v.refs.size(), v.refs_unreadable ? ", unreadable" : "");
        }
        for (const auto& t : lib.tls_variables) linef(out, "    {:#010x}  TLS variable, ref {:#010x}", t.nid, t.ref);
        for (const auto& p : lib.problems) linef(out, "    problem: {}", p);
    }

    usize nexp = 0;
    for (const auto& lib : m.exports) nexp += lib.functions.size() + lib.variables.size();
    linef(out, "exports ({} libraries, {} entries):", m.exports.size(), nexp);
    for (const auto& lib : m.exports) {
        linef(out, "  {}  version {:#06x}, attributes {:#06x}{}: {} functions, {} variables",
            lib.management ? std::string("(nameless: module management)") : library_label(lib.name), lib.version, lib.attributes,
            lib.skipped ? " (no library flag: ignored by the loader)" : "", lib.functions.size(), lib.variables.size());
        for (const auto& f : lib.functions) {
            linef(out, "    {:#010x}  {:<44}  descriptor {:#010x} -> entry {}, toc {}", f.nid, name_or_unknown(f.resolved), f.descriptor,
                opt_hex(f.entry), opt_hex(f.toc));
        }
        for (const auto& v : lib.variables) linef(out, "    {:#010x}  &{:<43}  at {:#010x}", v.nid, name_or_unknown(v.resolved), v.addr);
        for (const auto& p : lib.problems) linef(out, "    problem: {}", p);
    }

    const OpdInfo& opd = m.opd;
    if (opd.source == OpdInfo::Source::None) {
        linef(out, "function descriptors: not found{}", opd.entry_func ? std::format(" (entry point code {:#010x})", *opd.entry_func) : "");
    } else {
        linef(out, "function descriptors: {} entries ({} distinct functions, {} invalid) of {} bytes at [{:#010x}, {:#010x}) via {}{}",
            opd.entries.size(), opd.distinct_functions, opd.invalid_entries, opd.entry_size, opd.start, u64{opd.start} + opd.size,
            opd_source_name(opd.source), opd.section_name.empty() ? "" : " " + opd.section_name);
        linef(out, "                      toc {}, entry point code {}", opt_hex(opd.toc), opt_hex(opd.entry_func));
    }
}

void text_spu(std::string& out, const FileReport& r)
{
    u64 total = 0;
    for (const auto& p : r.spu_programs) total += p.image.size;
    linef(out, "SPU images: {} ({} total)", r.spu_programs.size(), bytes_text(total));
    for (usize i = 0; i < r.spu_programs.size(); i++) {
        const auto& p = r.spu_programs[i];
        const auto& img = p.image;
        linef(out, "  [{}] offset {:#010x}{}  size {:#x}{}  entry {:#07x}  {} segments, {} sections", i, img.offset,
            img.guest_addr ? std::format(" (addr {:#010x})", *img.guest_addr) : "", img.size, img.truncated ? " (TRUNCATED)" : "",
            img.entry, img.phnum, img.shnum);
        linef(out, "      sha1 {}  {}", to_hex(img.sha1), img.rpcs3_hash ? rpcs3_spu_hash_string(*img.rpcs3_hash) : "SPU-? (data missing)");
        std::string mw;
        for (const auto& h : p.middleware) mw += (mw.empty() ? "" : ", ") + std::string(h.product);
        linef(out, "      name {}  class {}{}", img.name.empty() ? "?" : std::format("'{}' ({})", img.name, img.name_source),
            spu_program_class_name(p.classification), mw.empty() ? "" : "  [" + mw + "]");
    }
}

void text_middleware(std::string& out, const FileReport& r)
{
    linef(out, "middleware signatures: {}", r.middleware.empty() ? std::string("none") : std::to_string(r.middleware.size()) + " products");
    for (const auto& h : r.middleware) {
        std::string patterns;
        for (const auto p : h.patterns) patterns += (patterns.empty() ? "" : ", ") + std::format("'{}'", p);
        linef(out, "  {:<11}  {:<24} {:>6} hits, first at {:#x}  ({})", h.origin == MiddlewareOrigin::Sony ? "Sony" : "third-party",
            h.product, h.count, h.first_offset, patterns);
    }
}

void text_summary(std::string& out, const FileReport& r)
{
    const Phase0Summary& s = r.phase0;
    linef(out, "---- Phase 0 summary: {} ----", r.path);
    if (r.encrypted()) {
        line(out, "  encrypted SELF: decrypt it with RPCS3 first; nothing else to report.");
        return;
    }
    linef(out, "  function count estimate: {}", s.function_estimate ? std::format("{} ({})", *s.function_estimate, s.function_estimate_source) : "unknown");
    u32 funcs = 0;
    u32 vars = 0;
    for (const auto& [mod, m] : s.imports) {
        funcs += m.functions;
        vars += m.variables;
    }
    linef(out, "  imported NIDs: {} functions + {} variables in {} modules", funcs, vars, s.imports.size());
    for (const auto& [mod, m] : s.imports) {
        linef(out, "    {:<28} {:>4} functions  {:>3} variables  {:>3} unresolved{}", library_label(mod), m.functions, m.variables,
            m.unresolved, m.other_module ? std::format("  ({} named via another module)", m.other_module) : "");
    }
    linef(out, "  unresolved NIDs: {}", s.unresolved.size());
    for (const auto& u : s.unresolved) linef(out, "    {}::{:#010x}{}", library_label(u.module), u.nid, u.variable ? " (variable)" : "");
    linef(out, "  SPU programs: {} = {} Sony middleware ({}) + {} third-party middleware ({}) + {} custom ({})", s.spu_programs,
        s.spu_sony, bytes_text(s.spu_bytes_sony), s.spu_third_party, bytes_text(s.spu_bytes_third_party), s.spu_custom,
        bytes_text(s.spu_bytes_custom));
}

} // namespace

std::string render_text(const FileReport& r, const ReportOptions& options)
{
    std::string out;
    linef(out, "==== {} ====", r.path);
    linef(out, "type             {}{}{}", file_kind_description(r.kind), r.encrypted() ? " (ENCRYPTED)" : "",
        r.detail.empty() ? "" : " - " + r.detail);
    linef(out, "size             {}", bytes_text(r.size));
    linef(out, "sha1             {}", to_hex(r.sha1));
    linef(out, "sha256           {}", to_hex(r.sha256));
    for (const auto& e : r.errors) linef(out, "ERROR: {}", e);

    if (r.self) text_self(out, *r.self);
    if (r.elf) {
        const ElfHeader& h = *r.elf;
        linef(out, "{:<17}class {}, type {:#06x} ({}), machine {} ({}), OS ABI {:#04x}, entry {:#x}, flags {:#x}",
            r.elf_offset ? std::format("ELF at {:#x}", r.elf_offset) : std::string("ELF"), h.is64() ? 64 : 32, h.type,
            elf::type_name(h.type), h.machine, elf::machine_name(h.machine), h.os_abi, h.entry, h.flags);
        if (r.rpcs3_ppu_hash) linef(out, "RPCS3 hash       {}", rpcs3_ppu_hash_string(*r.rpcs3_ppu_hash));
        if (r.symbol_count) linef(out, "symbols          {}", r.symbol_count);
        text_segments(out, r);
        text_sections(out, r);
    }
    if (r.ppu) text_ppu(out, *r.ppu);
    if (r.elf) text_spu(out, r);
    if (r.elf || r.kind == FileKind::Unknown) text_middleware(out, r);
    if (options.strings) {
        linef(out, "strings ({}):", r.strings.size());
        for (const auto& s : r.strings) linef(out, "  {:#010x}  {}", s.offset, s.text);
    }
    for (const auto& w : r.warnings) linef(out, "warning: {}", w);
    text_summary(out, r);
    return out;
}

// ---- JSON ----

namespace {

void json_resolved(JsonWriter& j, const ResolvedName& r)
{
    if (r.match == NidMatch::Unknown) {
        j.key("name").null();
    } else {
        j.field("name", r.name);
    }
    j.field("match", nid_match_name(r.match));
}

void json_strings(JsonWriter& j, std::string_view key, const std::vector<std::string>& list)
{
    j.key(key).begin_array();
    for (const auto& s : list) j.value(s);
    j.end_array();
}

void json_self(JsonWriter& j, const SelfInfo& s)
{
    j.key("self").begin_object();
    j.field("encrypted", s.encrypted).field("debug", s.debug);
    if (s.plaintext_elf_offset) j.field("plaintext_elf_offset", hex_compact(*s.plaintext_elf_offset));
    j.key("sce_header").begin_object();
    j.field("version", s.sce.version).field("key_revision", hex_compact(s.sce.key_revision)).field("type", s.sce.type);
    j.field("metadata_offset", hex_compact(s.sce.metadata_offset)).field("header_size", hex_compact(s.sce.header_size));
    j.field("data_size", hex_compact(s.sce.data_size));
    j.end_object();
    if (s.app) {
        j.key("app_info").begin_object();
        j.field("auth_id", std::format("{:#018x}", s.app->auth_id)).field("vendor_id", hex32(s.app->vendor_id));
        j.field("self_type", s.app->self_type).field("self_type_name", self_type_name(s.app->self_type));
        j.field("version", std::format("{:#018x}", s.app->version));
        j.end_object();
    }
    if (s.elf) {
        j.key("elf_header").begin_object();
        j.field("class", s.elf->is64() ? 64 : 32).field("type", hex_compact(s.elf->type)).field("type_name", elf::type_name(s.elf->type));
        j.field("machine", s.elf->machine).field("machine_name", elf::machine_name(s.elf->machine));
        j.field("entry", hex_compact(s.elf->entry)).field("phnum", s.elf->phnum).field("shnum", s.elf->shnum);
        j.end_object();
    }
    j.key("segments").begin_array();
    for (usize i = 0; i < s.segments.size(); i++) {
        const auto& seg = s.segments[i];
        j.begin_object();
        j.field("index", i).field("offset", hex_compact(seg.offset)).field("size", hex_compact(seg.size));
        j.field("compression", seg.compression).field("encryption", seg.encryption);
        if (i < s.phdrs.size()) {
            j.field("p_type", elf::program_type_name(s.phdrs[i].type)).field("p_vaddr", hex_compact(s.phdrs[i].vaddr));
            j.field("p_filesz", hex_compact(s.phdrs[i].filesz));
        }
        j.end_object();
    }
    j.end_array();
    j.key("control_info").begin_array();
    for (const auto& c : s.control) {
        j.begin_object();
        j.field("type", c.type).field("size", hex_compact(c.size));
        if (c.ctrl_flag1) j.field("ctrl_flag1", hex32(*c.ctrl_flag1));
        if (c.required_system_version) j.field("required_system_version", hex_compact(*c.required_system_version));
        if (c.npdrm) {
            j.key("npdrm").begin_object();
            j.field("content_id", c.npdrm->content_id).field("license", c.npdrm->license);
            j.field("license_name", npdrm_license_name(c.npdrm->license)).field("type", c.npdrm->type);
            j.field("version", c.npdrm->version);
            j.end_object();
        }
        j.end_object();
    }
    j.end_array();
    j.end_object();
}

void json_ppu(JsonWriter& j, const PpuModule& m)
{
    if (m.process_param) {
        const auto& p = *m.process_param;
        j.key("process_param").begin_object();
        j.field("vaddr", hex_compact(p.vaddr)).field("size", hex_compact(p.size)).field("magic_ok", p.magic_ok());
        j.field("version", hex32(p.version)).field("sdk_version", hex32(p.sdk_version)).field("primary_prio", p.primary_prio);
        j.field("primary_stacksize", hex_compact(p.primary_stacksize)).field("malloc_pagesize", hex_compact(p.malloc_pagesize));
        j.field("ppc_seg", p.ppc_seg);
        if (p.crash_dump_param_addr) j.field("crash_dump_param_addr", hex32(*p.crash_dump_param_addr));
        j.end_object();
    } else {
        j.key("process_param").null();
    }
    if (m.prx_param) {
        const auto& p = *m.prx_param;
        j.key("prx_param").begin_object();
        j.field("vaddr", hex_compact(p.vaddr)).field("magic_ok", p.magic_ok()).field("version", hex_compact(p.version));
        j.field("libent_start", hex32(p.libent_start)).field("libent_end", hex32(p.libent_end));
        j.field("libstub_start", hex32(p.libstub_start)).field("libstub_end", hex32(p.libstub_end)).field("ver", p.ver);
        j.end_object();
    }
    if (m.module_info) {
        const auto& mi = *m.module_info;
        j.key("module_info").begin_object();
        j.field("name", mi.name).field("version", std::format("{}.{}", mi.version_major, mi.version_minor));
        j.field("attributes", hex_compact(mi.attributes)).field("toc", hex32(mi.toc));
        j.field("exports_start", hex32(mi.exports_start)).field("exports_end", hex32(mi.exports_end));
        j.field("imports_start", hex32(mi.imports_start)).field("imports_end", hex32(mi.imports_end));
        j.field("vaddr", hex32(mi.vaddr)).field("located_by", mi.located_by);
        j.end_object();
    }
    if (m.tls) {
        j.key("tls").begin_object();
        j.field("vaddr", hex32(m.tls->vaddr)).field("image_size", m.tls->image_size).field("size", m.tls->size).field("align", m.tls->align);
        j.end_object();
    }
    j.key("relocations").begin_object();
    j.field("total", m.relocations.total).field("applied", m.relocations.applied);
    j.field("unsupported", m.relocations.unsupported).field("out_of_range", m.relocations.out_of_range);
    j.key("by_type").begin_object();
    for (const auto& [type, count] : m.relocations.by_type) j.field(std::to_string(type), count);
    j.end_object();
    j.end_object();

    j.key("imports").begin_array();
    for (const auto& lib : m.imports) {
        j.begin_object();
        j.field("module", lib.name).field("addr", hex32(lib.addr)).field("version", hex_compact(lib.version));
        j.field("attributes", hex_compact(lib.attributes)).field("struct_size", lib.struct_size);
        j.key("functions").begin_array();
        for (const auto& f : lib.functions) {
            j.begin_object();
            j.field("nid", hex32(f.nid));
            json_resolved(j, f.resolved);
            j.field("stub", hex32(f.stub)).field("slot", hex32(f.slot));
            if (f.refs) j.field("refs", hex32(*f.refs));
            j.end_object();
        }
        j.end_array();
        j.key("variables").begin_array();
        for (const auto& v : lib.variables) {
            j.begin_object();
            j.field("nid", hex32(v.nid));
            json_resolved(j, v.resolved);
            j.field("ref_table", hex32(v.ref_table)).field("refs_unreadable", v.refs_unreadable);
            j.key("refs").begin_array();
            for (const auto& ref : v.refs) {
                j.begin_object().field("type", ref.type).field("addr", hex32(ref.addr)).field("addend", hex32(ref.addend)).end_object();
            }
            j.end_array();
            j.end_object();
        }
        j.end_array();
        j.field("tls_variable_count", lib.num_tlsvar);
        j.key("tls_variables").begin_array();
        for (const auto& t : lib.tls_variables) j.begin_object().field("nid", hex32(t.nid)).field("ref", hex32(t.ref)).end_object();
        j.end_array();
        json_strings(j, "problems", lib.problems);
        j.end_object();
    }
    j.end_array();

    j.key("exports").begin_array();
    for (const auto& lib : m.exports) {
        j.begin_object();
        j.field("module", lib.name).field("management", lib.management).field("skipped", lib.skipped);
        j.field("addr", hex32(lib.addr)).field("version", hex_compact(lib.version)).field("attributes", hex_compact(lib.attributes));
        j.key("functions").begin_array();
        for (const auto& f : lib.functions) {
            j.begin_object();
            j.field("nid", hex32(f.nid));
            json_resolved(j, f.resolved);
            j.field("descriptor", hex32(f.descriptor));
            if (f.entry) j.field("entry", hex32(*f.entry));
            if (f.toc) j.field("toc", hex32(*f.toc));
            j.end_object();
        }
        j.end_array();
        j.key("variables").begin_array();
        for (const auto& v : lib.variables) {
            j.begin_object();
            j.field("nid", hex32(v.nid));
            json_resolved(j, v.resolved);
            j.field("addr", hex32(v.addr));
            j.end_object();
        }
        j.end_array();
        json_strings(j, "problems", lib.problems);
        j.end_object();
    }
    j.end_array();

    const OpdInfo& opd = m.opd;
    j.key("opd").begin_object();
    j.field("source", opd_source_name(opd.source)).field("section", opd.section_name);
    j.field("start", hex32(opd.start)).field("size", hex_compact(opd.size)).field("entry_size", opd.entry_size);
    j.field("count", opd.entries.size()).field("distinct_functions", opd.distinct_functions).field("invalid_entries", opd.invalid_entries);
    if (opd.toc) {
        j.field("toc", hex32(*opd.toc));
    } else {
        j.key("toc").null();
    }
    if (opd.entry_func) {
        j.field("entry_func", hex32(*opd.entry_func));
    } else {
        j.key("entry_func").null();
    }
    j.end_object();
}

void json_spu(JsonWriter& j, const FileReport& r)
{
    u64 total = 0;
    for (const auto& p : r.spu_programs) total += p.image.size;
    j.key("spu_images").begin_object();
    j.field("count", r.spu_programs.size()).field("total_bytes", total);
    j.key("images").begin_array();
    for (usize i = 0; i < r.spu_programs.size(); i++) {
        const auto& p = r.spu_programs[i];
        const auto& img = p.image;
        j.begin_object();
        j.field("index", i).field("offset", hex_compact(img.offset));
        if (img.guest_addr) {
            j.field("guest_addr", hex32(*img.guest_addr));
        } else {
            j.key("guest_addr").null();
        }
        j.field("size", img.size).field("truncated", img.truncated).field("sha1", to_hex(img.sha1));
        if (img.rpcs3_hash) {
            j.field("rpcs3_hash", rpcs3_spu_hash_string(*img.rpcs3_hash));
        } else {
            j.key("rpcs3_hash").null();
        }
        j.field("entry", hex32(img.entry)).field("elf_type", img.type).field("phnum", img.phnum).field("shnum", img.shnum);
        j.field("load_bytes", img.load_bytes).field("name", img.name).field("name_source", img.name_source);
        j.field("class", spu_program_class_name(p.classification));
        j.key("middleware").begin_array();
        for (const auto& h : p.middleware) j.value(h.product);
        j.end_array();
        j.key("segments").begin_array();
        for (const auto& s : img.segments) {
            j.begin_object().field("type", elf::program_type_name(s.type)).field("vaddr", hex32(static_cast<u32>(s.vaddr)));
            j.field("filesz", s.filesz).field("memsz", s.memsz).field("flags", elf::segment_flags_string(s.flags)).end_object();
        }
        j.end_array();
        json_strings(j, "warnings", img.warnings);
        j.end_object();
    }
    j.end_array();
    j.end_object();
}

void json_phase0(JsonWriter& j, const FileReport& r)
{
    const Phase0Summary& s = r.phase0;
    j.key("phase0").begin_object();
    if (s.function_estimate) {
        j.field("function_estimate", *s.function_estimate);
    } else {
        j.key("function_estimate").null();
    }
    j.field("function_estimate_source", s.function_estimate_source);
    u32 funcs = 0;
    u32 vars = 0;
    j.key("imports").begin_array();
    for (const auto& [mod, m] : s.imports) {
        funcs += m.functions;
        vars += m.variables;
        j.begin_object().field("module", mod).field("functions", m.functions).field("variables", m.variables);
        j.field("unresolved", m.unresolved).field("other_module", m.other_module).end_object();
    }
    j.end_array();
    j.field("imported_functions", funcs).field("imported_variables", vars);
    j.key("unresolved").begin_array();
    for (const auto& u : s.unresolved) {
        j.begin_object().field("module", u.module).field("nid", hex32(u.nid)).field("kind", u.variable ? "variable" : "function").end_object();
    }
    j.end_array();
    j.key("spu").begin_object();
    j.field("total", s.spu_programs).field("sony_middleware", s.spu_sony).field("third_party_middleware", s.spu_third_party);
    j.field("custom", s.spu_custom).field("bytes_sony_middleware", s.spu_bytes_sony);
    j.field("bytes_third_party_middleware", s.spu_bytes_third_party).field("bytes_custom", s.spu_bytes_custom);
    j.end_object();
    j.end_object();
}

} // namespace

void write_json(JsonWriter& j, const FileReport& r, const ReportOptions& options)
{
    j.begin_object();
    j.field("path", r.path).field("size", r.size).field("sha1", to_hex(r.sha1)).field("sha256", to_hex(r.sha256));
    j.field("type", file_kind_name(r.kind)).field("type_description", file_kind_description(r.kind)).field("detail", r.detail);
    j.field("encrypted", r.encrypted());
    json_strings(j, "errors", r.errors);
    json_strings(j, "warnings", r.warnings);

    if (r.self) json_self(j, *r.self);
    if (r.elf) {
        const ElfHeader& h = *r.elf;
        j.key("elf").begin_object();
        j.field("offset", hex_compact(r.elf_offset)).field("class", h.is64() ? 64 : 32).field("type", hex_compact(h.type));
        j.field("type_name", elf::type_name(h.type)).field("machine", h.machine).field("machine_name", elf::machine_name(h.machine));
        j.field("os_abi", hex_compact(h.os_abi)).field("entry", hex_compact(h.entry)).field("flags", hex_compact(h.flags));
        j.field("phnum", h.phnum).field("shnum", h.shnum).field("symbols", r.symbol_count);
        if (r.rpcs3_ppu_hash) j.field("rpcs3_hash", rpcs3_ppu_hash_string(*r.rpcs3_ppu_hash));
        j.end_object();

        j.key("segments").begin_array();
        for (usize i = 0; i < r.segments.size(); i++) {
            const auto& p = r.segments[i];
            j.begin_object();
            j.field("index", i).field("type", hex32(p.type)).field("type_name", elf::program_type_name(p.type));
            j.field("flags", hex32(p.flags)).field("flags_text", elf::segment_flags_string(p.flags));
            j.field("offset", hex_compact(p.offset)).field("vaddr", hex_compact(p.vaddr)).field("paddr", hex_compact(p.paddr));
            j.field("filesz", hex_compact(p.filesz)).field("memsz", hex_compact(p.memsz)).field("align", hex_compact(p.align));
            j.field("in_file", p.data_in_file);
            j.end_object();
        }
        j.end_array();
        j.key("sections").begin_array();
        for (usize i = 0; i < r.sections.size(); i++) {
            const auto& s = r.sections[i];
            j.begin_object();
            j.field("index", i).field("name", s.name).field("type", hex32(s.type)).field("type_name", elf::section_type_name(s.type));
            j.field("flags", hex_compact(s.flags)).field("addr", hex_compact(s.addr)).field("offset", hex_compact(s.offset));
            j.field("size", hex_compact(s.size)).field("in_file", s.data_in_file);
            j.end_object();
        }
        j.end_array();
    }
    if (r.ppu) json_ppu(j, *r.ppu);
    if (r.elf) json_spu(j, r);

    j.key("middleware").begin_array();
    for (const auto& h : r.middleware) {
        j.begin_object();
        j.field("product", h.product).field("vendor", h.vendor).field("origin", middleware_origin_name(h.origin));
        j.field("count", h.count).field("first_offset", hex_compact(h.first_offset));
        j.key("patterns").begin_array();
        for (const auto p : h.patterns) j.value(p);
        j.end_array();
        j.end_object();
    }
    j.end_array();

    if (options.strings) {
        j.key("strings").begin_array();
        for (const auto& s : r.strings) j.begin_object().field("offset", hex_compact(s.offset)).field("text", s.text).end_object();
        j.end_array();
    }
    json_phase0(j, r);
    j.end_object();
}

} // namespace pspocket::ps3inspect

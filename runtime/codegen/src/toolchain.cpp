#include "pspocket/codegen/toolchain.hpp"

#include <mutex>
#include <optional>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#endif

#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/DiagnosticOptions.h>
#include <clang/Basic/Stack.h>
#include <clang/CodeGen/CodeGenAction.h>
#include <clang/Driver/Compilation.h>
#include <clang/Driver/Driver.h>
#include <clang/Driver/Job.h>
#include <clang/Driver/Tool.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/CompilerInvocation.h>
#include <clang/Frontend/TextDiagnosticPrinter.h>
#include <lld/Common/Driver.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/Support/thread.h>

LLD_HAS_DRIVER(macho)

extern "C" {
void LLVMInitializeAArch64TargetInfo();
void LLVMInitializeAArch64Target();
void LLVMInitializeAArch64TargetMC();
void LLVMInitializeAArch64AsmPrinter();
void LLVMInitializeAArch64AsmParser();
}

namespace pspocket::codegen {
namespace {

void init_llvm()
{
    static std::once_flag once;
    std::call_once(once, [] {
        LLVMInitializeAArch64TargetInfo();
        LLVMInitializeAArch64Target();
        LLVMInitializeAArch64TargetMC();
        LLVMInitializeAArch64AsmPrinter();
        LLVMInitializeAArch64AsmParser();
    });
}

std::vector<const char*> pointers(const std::vector<std::string>& args)
{
    std::vector<const char*> out;
    out.reserve(args.size());
    for (const std::string& a : args) out.push_back(a.c_str());
    return out;
}

bool compile_here(const std::string& source, const std::string& object, const CompileOptions& o,
    std::string* diagnostics)
{
    // The driver's arguments, as `clang -c` takes them: the driver turns them into the compile's own (cc1) settings.
    std::vector<std::string> args = {"clang", "-target", o.target, "-mcpu=" + (o.cpu == "native" ? host_cpu() : o.cpu),
        "-c", source, "-o", object,
        "-O" + std::to_string(o.opt_level), "-std=gnu11", "-g0", "-w", "-ffp-contract=off", "-fno-fast-math",
        "-fno-stack-protector", "-fno-common", "-nostdlibinc"};
    if (!o.resource_dir.empty()) {
        args.push_back("-resource-dir");
        args.push_back(o.resource_dir);
    }
    for (const std::string& d : o.system_dirs) {
        args.push_back("-isystem");
        args.push_back(d);
    }
    // Empty entries are skipped: "-D" alone would take the next argument as the macro.
    for (const std::string& d : o.include_dirs)
        if (!d.empty()) args.push_back("-I" + d);
    for (const std::string& d : o.defines)
        if (!d.empty()) args.push_back("-D" + d);
    args.insert(args.end(), o.extra_args.begin(), o.extra_args.end());
    const auto argv = pointers(args);

    std::string text;
    llvm::raw_string_ostream os(text);
    clang::DiagnosticOptions diag_options;
    auto* printer = new clang::TextDiagnosticPrinter(os, diag_options);
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fs = llvm::vfs::getRealFileSystem();
    llvm::IntrusiveRefCntPtr<clang::DiagnosticsEngine> diags =
        clang::CompilerInstance::createDiagnostics(*fs, diag_options, printer, true);
    // The compile clang's driver makes of these arguments: its one job, `clang -cc1 ... -emit-obj -o object`.
    // (clang::createInvocation would do this too, but it adds -fsyntax-only, which drops -o: the object would land
    // beside the source.)
    std::shared_ptr<clang::CompilerInvocation> invocation;
    clang::driver::Driver driver(args[0], o.target, *diags, "pspocket", fs);
    std::unique_ptr<clang::driver::Compilation> compilation(driver.BuildCompilation(argv));
    if (compilation && !compilation->containsError()) {
        const clang::driver::JobList& jobs = compilation->getJobs();
        if (jobs.size() == 1 && llvm::StringRef(jobs.begin()->getCreator().getName()) == "clang") {
            invocation = std::make_shared<clang::CompilerInvocation>();
            if (!clang::CompilerInvocation::CreateFromArgs(*invocation, jobs.begin()->getArguments(), *diags, argv[0]))
                invocation.reset();
        } else {
            jobs.Print(os, "; ", true);
            os << "\nexpected one compile job\n";
        }
    }
    bool ok = false;
    if (invocation && invocation->getFrontendOpts().OutputFile == object) {
        // The driver tells the compile to leave its memory unfreed (clang exits right after); this process stays.
        invocation->getFrontendOpts().DisableFree = false;
        invocation->getCodeGenOpts().DisableFree = false;
        clang::CompilerInstance compiler(std::move(invocation));
        compiler.createDiagnostics(new clang::TextDiagnosticPrinter(os, compiler.getDiagnosticOpts()), true);
        clang::EmitObjAction action;
        ok = compiler.ExecuteAction(action) && !compiler.getDiagnostics().hasErrorOccurred();
    }
    os.flush();
    if (diagnostics) *diagnostics = text;
    return ok;
}

} // namespace

bool compile_c(const std::string& source, const std::string& object, const CompileOptions& o, std::string* diagnostics)
{
    init_llvm();
    // On a thread of its own with the stack clang's driver gives itself (8 MB, clang::DesiredStackSize): iOS gives
    // threads other than the main one 512 KB, and clang recurses through large functions.
    bool ok = false;
    llvm::thread worker(std::optional<unsigned>(clang::DesiredStackSize), [&] {
        clang::noteBottomOfStack();
        ok = compile_here(source, object, o, diagnostics);
    });
    worker.join();
#if defined(__APPLE__)
    // The allocator keeps freed pages, and iOS counts them against the app (its footprint) until they go back.
    malloc_zone_pressure_relief(nullptr, 0);
#endif
    return ok;
}

std::string compiler_version() { return "LLVM " LLVM_VERSION_STRING; }

std::string host_cpu() { return llvm::sys::getHostCPUName().str(); }

bool set_llvm_options(const std::vector<std::string>& options, std::string* diagnostics)
{
    std::vector<std::string> args = {"pspocket-codegen"};
    args.insert(args.end(), options.begin(), options.end());
    const auto argv = pointers(args);
    std::string text;
    llvm::raw_string_ostream os(text);
    const bool ok = llvm::cl::ParseCommandLineOptions(static_cast<int>(argv.size()), argv.data(), "", &os);
    os.flush();
    if (diagnostics) *diagnostics = text;
    return ok;
}

bool link_dylib(const std::vector<std::string>& objects, const std::string& output, const LinkOptions& o,
    std::string* diagnostics)
{
    static std::mutex one_at_a_time; // lld keeps global state through a link
    std::lock_guard lock(one_at_a_time);
    std::vector<std::string> args = {"ld64.lld", "-arch", "arm64", "-platform_version", o.platform, o.min_os, o.min_os,
        "-dylib", "-o", output, "-install_name", o.install_name, "-undefined", "error",
        "-adhoc_codesign"}; // the signature's room for codesign::sign (lld reserves it unasked only for macOS)
    for (const std::string& e : o.exports) {
        args.push_back("-exported_symbol");
        args.push_back(e);
    }
    for (const std::string& obj : objects) args.push_back(obj);
    for (const std::string& lib : o.libraries) args.push_back(lib);
    const auto argv = pointers(args);
    std::string out_text, err_text;
    llvm::raw_string_ostream out(out_text), err(err_text);
    const lld::Result r = lld::lldMain(argv, out, err, {{lld::Darwin, &lld::macho::link}});
    out.flush();
    err.flush();
    if (diagnostics) *diagnostics = out_text + err_text;
    return r.retCode == 0;
}

} // namespace pspocket::codegen

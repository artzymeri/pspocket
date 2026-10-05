#pragma once

#include <format>
#include <span>
#include <string>
#include <string_view>

#include "pspocket/selftest.h"

namespace pspocket::selftest {

// Passed to every probe. Reporting never throws; a probe keeps going after a failure unless it returns.
class ProbeContext {
public:
    ProbeContext(const char* name, ps_selftest_callback callback, void* user)
        : name_(name), callback_(callback), user_(user)
    {
    }

    template <class... Args>
    void info(std::format_string<Args...> fmt, Args&&... args)
    {
        emit(PS_SELFTEST_INFO, std::format(fmt, std::forward<Args>(args)...));
    }

    // One measurement, "name = value unit". Hosts parse nothing; the format is for people and logs.
    template <class... Args>
    void metric(std::format_string<Args...> fmt, Args&&... args)
    {
        emit(PS_SELFTEST_METRIC, std::format(fmt, std::forward<Args>(args)...));
    }

    template <class... Args>
    void fail(std::format_string<Args...> fmt, Args&&... args)
    {
        failed_ = true;
        emit(PS_SELFTEST_INFO, "FAIL: " + std::format(fmt, std::forward<Args>(args)...));
    }

    // Fails the probe when cond is false. Returns cond so callers can bail out.
    bool check(bool cond, std::string_view what)
    {
        if (!cond) fail("{}", what);
        return cond;
    }

    void skip(std::string_view why)
    {
        skipped_ = true;
        skip_reason_ = why;
    }

    void set_summary(std::string summary) { summary_ = std::move(summary); }

    bool failed() const { return failed_; }
    bool skipped() const { return skipped_; }
    const std::string& summary() const { return summary_; }
    const std::string& skip_reason() const { return skip_reason_; }
    const char* name() const { return name_; }

    void emit(ps_selftest_event event, const std::string& message) const
    {
        if (callback_) callback_(user_, event, name_, message.c_str());
    }

private:
    const char* name_;
    ps_selftest_callback callback_;
    void* user_;
    bool failed_ = false;
    bool skipped_ = false;
    std::string summary_;
    std::string skip_reason_;
};

struct Probe {
    const char* name;
    const char* description;
    void (*run)(ProbeContext&);
};

std::span<const Probe> all_probes();

// Probe implementations, grouped by component (probes_<component>.cpp).
void probe_platform(ProbeContext&);
void probe_mem_reserve(ProbeContext&);
void probe_mem_bandwidth(ProbeContext&);
void probe_mem_fault_tracking(ProbeContext&);
void probe_timebase(ProbeContext&);
void probe_threads_qos(ProbeContext&);
void probe_ppu_interp(ProbeContext&); // probes_ppu.cpp
void probe_ppu_lifted(ProbeContext&);
void probe_codegen_dlopen(ProbeContext&); // probes_codegen.cpp: route B's go/no-go (D-150)

// True when a debugger is attached (intentional faults would stop in LLDB).
bool debugger_attached();

} // namespace pspocket::selftest

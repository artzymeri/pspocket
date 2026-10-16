#include "pspocket/core/thread_report.hpp"

#include <algorithm>
#include <format>
#include <unordered_map>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace pspocket {

std::string thread_cpu_report(double seconds, usize top)
{
#if defined(__APPLE__)
    static std::unordered_map<u64, u64> last; // thread ID -> its CPU nanoseconds at the previous call
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) return {};
    struct Row {
        std::string name;
        u64 ns;
    };
    std::vector<Row> rows;
    std::unordered_map<u64, u64> now;
    u64 total = 0;
    for (mach_msg_type_number_t i = 0; i < count; i++) {
        thread_identifier_info_data_t id{};
        mach_msg_type_number_t id_count = THREAD_IDENTIFIER_INFO_COUNT;
        thread_extended_info_data_t ext{};
        mach_msg_type_number_t ext_count = THREAD_EXTENDED_INFO_COUNT;
        if (thread_info(threads[i], THREAD_IDENTIFIER_INFO, reinterpret_cast<thread_info_t>(&id), &id_count) ==
                KERN_SUCCESS &&
            thread_info(threads[i], THREAD_EXTENDED_INFO, reinterpret_cast<thread_info_t>(&ext), &ext_count) ==
                KERN_SUCCESS) {
            const u64 ns = ext.pth_user_time + ext.pth_system_time;
            now[id.thread_id] = ns;
            const auto it = last.find(id.thread_id);
            const u64 used = ns - (it != last.end() ? std::min(it->second, ns) : 0);
            total += used;
            rows.push_back({ext.pth_name[0] ? std::string(ext.pth_name) : std::format("thread {}", id.thread_id), used});
        }
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), count * sizeof(thread_act_t));
    last = std::move(now);
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.ns > b.ns; });
    std::string out = std::format("CPU {:.1f} s in {:.1f} s ({:.1f} cores), {} threads", static_cast<double>(total) / 1e9,
        seconds, seconds > 0 ? static_cast<double>(total) / 1e9 / seconds : 0.0, count);
    for (usize i = 0; i < rows.size() && i < top && rows[i].ns >= 10'000'000; i++) {
        out += std::format("{} {} {:.2f} s", i ? "," : ":", rows[i].name, static_cast<double>(rows[i].ns) / 1e9);
    }
    return out;
#else
    (void)seconds;
    (void)top;
    return {};
#endif
}

std::vector<ThreadCpu> thread_cpu_times()
{
    std::vector<ThreadCpu> out;
#if defined(__APPLE__)
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) return out;
    out.reserve(count);
    for (mach_msg_type_number_t i = 0; i < count; i++) {
        thread_identifier_info_data_t id{};
        mach_msg_type_number_t id_count = THREAD_IDENTIFIER_INFO_COUNT;
        thread_extended_info_data_t ext{};
        mach_msg_type_number_t ext_count = THREAD_EXTENDED_INFO_COUNT;
        if (thread_info(threads[i], THREAD_IDENTIFIER_INFO, reinterpret_cast<thread_info_t>(&id), &id_count) ==
                KERN_SUCCESS &&
            thread_info(threads[i], THREAD_EXTENDED_INFO, reinterpret_cast<thread_info_t>(&ext), &ext_count) ==
                KERN_SUCCESS) {
            out.push_back({id.thread_id, ext.pth_name, ext.pth_user_time + ext.pth_system_time});
        }
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), count * sizeof(thread_act_t));
#endif
    return out;
}

} // namespace pspocket

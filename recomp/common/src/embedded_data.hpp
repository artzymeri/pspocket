#pragma once

#include <cstddef>
#include <string_view>

// Data files compiled into the library (CMakeLists.txt, cmake/embed_file.cmake).
namespace pspocket::recomp::embedded {

extern const unsigned char k_nids_txt[];
extern const std::size_t k_nids_txt_size;
extern const unsigned char k_syscalls_txt[];
extern const std::size_t k_syscalls_txt_size;

inline std::string_view nids_txt() { return {reinterpret_cast<const char*>(k_nids_txt), k_nids_txt_size}; }
inline std::string_view syscalls_txt() { return {reinterpret_cast<const char*>(k_syscalls_txt), k_syscalls_txt_size}; }

} // namespace pspocket::recomp::embedded

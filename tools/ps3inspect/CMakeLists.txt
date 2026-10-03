# ps3inspect: the Phase 0 inventory tool (plan section 10). Usage: ps3inspect --help.
add_executable(ps3inspect main.cpp report.cpp)
target_link_libraries(ps3inspect PRIVATE pspocket::recomp_common)
pspocket_set_warnings(ps3inspect)

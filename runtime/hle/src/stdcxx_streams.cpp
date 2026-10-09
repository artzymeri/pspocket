// sys_libstdcxx, the foundation and the streams (D-560): the object model (stdcxx.hpp), operator new and delete, the
// exceptions, the strings and _Mutex (stdcxx_core.hpp), the locale (stdcxx_locale.hpp), ios_base, basic_ios, the stream
// buffers and streams (stdcxx_ios.hpp); this file has the classes' vtables and type_info and registers the functions.
// The facets are the other part's (stdcxx_facets.cpp, D-561), reached through the registry.

#include "stdcxx_core.hpp"
#include "stdcxx_ios.hpp"
#include "stdcxx_locale.hpp"

namespace pspocket::hle {

namespace stdcxx {

// ------------------------------------------------------------------------------------------------- vtables

namespace {

constexpr std::string_view kSbC = "St15basic_streambufIcSt11char_traitsIcEE",
                           kSbW = "St15basic_streambufIwSt11char_traitsIwEE",
                           kFbC = "St13basic_filebufIcSt11char_traitsIcEE",
                           kFbW = "St13basic_filebufIwSt11char_traitsIwEE",
                           kIosC = "St9basic_iosIcSt11char_traitsIcEE", kIosW = "St9basic_iosIwSt11char_traitsIwEE",
                           kIsW = "St13basic_istreamIwSt11char_traitsIwEE",
                           kOsW = "St13basic_ostreamIwSt11char_traitsIwEE";

// An exception class's vtable: [~D1, ~D0, what, _Doraise].
VtableDef exception_vtable(std::string_view cls, std::string_view d1, std::string_view d0, std::string_view what,
    std::string_view doraise)
{
    return {cls, {V(0), T(cls), F(d1), F(d0), F(what), F(doraise)}, {2}};
}

// A class whose only virtual function is its destructor.
VtableDef dtor_vtable(std::string_view cls, std::string_view d1, std::string_view d0)
{
    return {cls, {V(0), T(cls), F(d1), F(d0)}, {2}};
}

// A stream buffer's: [~D1, ~D0, _Lock, _Unlock, overflow, pbackfail, showmanyc, underflow, uflow, xsgetn, xsputn,
// seekoff, seekpos, setbuf, sync, imbue].
VtableDef streambuf_vtable(std::string_view cls, std::initializer_list<std::string_view> slots)
{
    VtableDef d{cls, {V(0), T(cls)}, {2}};
    for (const std::string_view s : slots) d.words.push_back(F(s));
    return d;
}

// istream-like and ostream-like classes (one virtual base): [vbase, 0, ti, ~D1, ~D0], then the basic_ios-in-X group
// [vcall, -vbase, ti, the virtual thunks].
VtableDef stream_vtable(std::string_view cls, s32 vbase, std::string_view d1, std::string_view d0,
    std::string_view t1, std::string_view t0)
{
    return {cls, {V(vbase), V(0), T(cls), F(d1), F(d0), V(-vbase), V(-vbase), T(cls), F(t1), F(t0)}, {3, 8}};
}

// iostream-like classes: the istream part's group, the ostream-in-X group at +8 (non-virtual thunks), then
// basic_ios-in-X.
VtableDef iostream_vtable(std::string_view cls, s32 vbase, std::string_view d1, std::string_view d0,
    std::string_view n1, std::string_view n0, std::string_view t1, std::string_view t0)
{
    return {cls,
        {V(vbase), V(0), T(cls), F(d1), F(d0), V(vbase - 8), V(-8), T(cls), F(n1), F(n0), V(-vbase), V(-vbase), T(cls),
            F(t1), F(t0)},
        {3, 8, 13}};
}

} // namespace

const std::vector<VtableDef>& vtable_defs()
{
    static const std::vector<VtableDef> defs = [] {
        std::vector<VtableDef> v;
        v.push_back(exception_vtable("St9exception", "_ZNSt9exceptionD1Ev", "_ZNSt9exceptionD0Ev",
            "_ZNKSt9exception4whatEv", "_ZNKSt9exception8_DoraiseEv"));
        v.push_back(exception_vtable("St9bad_alloc", "_ZNSt9bad_allocD1Ev", "_ZNSt9bad_allocD0Ev",
            "_ZNKSt9bad_alloc4whatEv", "_ZNKSt9bad_alloc8_DoraiseEv"));
        v.push_back(exception_vtable("St8bad_cast", "_ZNSt8bad_castD1Ev", "_ZNSt8bad_castD0Ev", "_ZNKSt8bad_cast4whatEv",
            "_ZNKSt9exception8_DoraiseEv"));
        v.push_back(exception_vtable("St11logic_error", "_ZNSt11logic_errorD1Ev", "_ZNSt11logic_errorD0Ev",
            "_ZNKSt11logic_error4whatEv", "_ZNKSt11logic_error8_DoraiseEv"));
        v.push_back(exception_vtable("St12length_error", "_ZNSt12length_errorD1Ev", "_ZNSt12length_errorD0Ev",
            "_ZNKSt11logic_error4whatEv", "_ZNKSt12length_error8_DoraiseEv"));
        v.push_back(exception_vtable("St12out_of_range", "_ZNSt12out_of_rangeD1Ev", "_ZNSt12out_of_rangeD0Ev",
            "_ZNKSt11logic_error4whatEv", "_ZNKSt12out_of_range8_DoraiseEv"));
        v.push_back(exception_vtable("St13runtime_error", "_ZNSt13runtime_errorD1Ev", "_ZNSt13runtime_errorD0Ev",
            "_ZNKSt13runtime_error4whatEv", "_ZNKSt13runtime_error8_DoraiseEv"));
        v.push_back(exception_vtable("NSt8ios_base7failureE", "_ZNSt8ios_base7failureD1Ev",
            "_ZNSt8ios_base7failureD0Ev", "_ZNKSt13runtime_error4whatEv", "_ZNKSt8ios_base7failure8_DoraiseEv"));
        v.push_back(dtor_vtable("NSt6locale5facetE", "_ZNSt6locale5facetD1Ev", "_ZNSt6locale5facetD0Ev"));
        v.push_back(dtor_vtable("NSt6locale7_LocimpE", "_ZNSt6locale7_LocimpD1Ev", "_ZNSt6locale7_LocimpD0Ev"));
        v.push_back(dtor_vtable("St8ios_base", "_ZNSt8ios_baseD1Ev", "_ZNSt8ios_baseD0Ev"));
        v.push_back(dtor_vtable(kIosC, "_ZNSt9basic_iosIcSt11char_traitsIcEED1Ev",
            "_ZNSt9basic_iosIcSt11char_traitsIcEED0Ev"));
        v.push_back(dtor_vtable(kIosW, "_ZNSt9basic_iosIwSt11char_traitsIwEED1Ev",
            "_ZNSt9basic_iosIwSt11char_traitsIwEED0Ev"));
        v.push_back(streambuf_vtable(kSbC,
            {"_ZNSt15basic_streambufIcSt11char_traitsIcEED1Ev", "_ZNSt15basic_streambufIcSt11char_traitsIcEED0Ev",
                "_ZNSt15basic_streambufIcSt11char_traitsIcEE5_LockEv",
                "_ZNSt15basic_streambufIcSt11char_traitsIcEE7_UnlockEv",
                "_ZNSt15basic_streambufIcSt11char_traitsIcEE8overflowEi",
                "_ZNSt15basic_streambufIcSt11char_traitsIcEE9pbackfailEi",
                "_ZNSt15basic_streambufIcSt11char_traitsIcEE9showmanycEv",
                "_ZNSt15basic_streambufIcSt11char_traitsIcEE9underflowEv",
                "_ZNSt15basic_streambufIcSt11char_traitsIcEE5uflowEv",
                "_ZNSt15basic_streambufIcSt11char_traitsIcEE6xsgetnEPci",
                "_ZNSt15basic_streambufIcSt11char_traitsIcEE6xsputnEPKci",
                "_ZNSt15basic_streambufIcSt11char_traitsIcEE7seekoffElNSt5_IosbIiE8_SeekdirENS4_9_OpenmodeE",
                "_ZNSt15basic_streambufIcSt11char_traitsIcEE7seekposESt4fposISt9_MbstatetENSt5_IosbIiE9_OpenmodeE",
                "_ZNSt15basic_streambufIcSt11char_traitsIcEE6setbufEPci",
                "_ZNSt15basic_streambufIcSt11char_traitsIcEE4syncEv",
                "_ZNSt15basic_streambufIcSt11char_traitsIcEE5imbueERKSt6locale"}));
        v.push_back(streambuf_vtable(kSbW,
            {"_ZNSt15basic_streambufIwSt11char_traitsIwEED1Ev", "_ZNSt15basic_streambufIwSt11char_traitsIwEED0Ev",
                "_ZNSt15basic_streambufIwSt11char_traitsIwEE5_LockEv",
                "_ZNSt15basic_streambufIwSt11char_traitsIwEE7_UnlockEv",
                "_ZNSt15basic_streambufIwSt11char_traitsIwEE8overflowEi",
                "_ZNSt15basic_streambufIwSt11char_traitsIwEE9pbackfailEi",
                "_ZNSt15basic_streambufIwSt11char_traitsIwEE9showmanycEv",
                "_ZNSt15basic_streambufIwSt11char_traitsIwEE9underflowEv",
                "_ZNSt15basic_streambufIwSt11char_traitsIwEE5uflowEv",
                "_ZNSt15basic_streambufIwSt11char_traitsIwEE6xsgetnEPwi",
                "_ZNSt15basic_streambufIwSt11char_traitsIwEE6xsputnEPKwi",
                "_ZNSt15basic_streambufIwSt11char_traitsIwEE7seekoffElNSt5_IosbIiE8_SeekdirENS4_9_OpenmodeE",
                "_ZNSt15basic_streambufIwSt11char_traitsIwEE7seekposESt4fposISt9_MbstatetENSt5_IosbIiE9_OpenmodeE",
                "_ZNSt15basic_streambufIwSt11char_traitsIwEE6setbufEPwi",
                "_ZNSt15basic_streambufIwSt11char_traitsIwEE4syncEv",
                "_ZNSt15basic_streambufIwSt11char_traitsIwEE5imbueERKSt6locale"}));
        v.push_back(streambuf_vtable(kFbC,
            {"_ZNSt13basic_filebufIcSt11char_traitsIcEED1Ev", "_ZNSt13basic_filebufIcSt11char_traitsIcEED0Ev",
                "_ZNSt13basic_filebufIcSt11char_traitsIcEE5_LockEv",
                "_ZNSt13basic_filebufIcSt11char_traitsIcEE7_UnlockEv",
                "_ZNSt13basic_filebufIcSt11char_traitsIcEE8overflowEi",
                "_ZNSt13basic_filebufIcSt11char_traitsIcEE9pbackfailEi",
                "_ZNSt15basic_streambufIcSt11char_traitsIcEE9showmanycEv",
                "_ZNSt13basic_filebufIcSt11char_traitsIcEE9underflowEv",
                "_ZNSt13basic_filebufIcSt11char_traitsIcEE5uflowEv",
                "_ZNSt15basic_streambufIcSt11char_traitsIcEE6xsgetnEPci",
                "_ZNSt15basic_streambufIcSt11char_traitsIcEE6xsputnEPKci",
                "_ZNSt13basic_filebufIcSt11char_traitsIcEE7seekoffElNSt5_IosbIiE8_SeekdirENS4_9_OpenmodeE",
                "_ZNSt13basic_filebufIcSt11char_traitsIcEE7seekposESt4fposISt9_MbstatetENSt5_IosbIiE9_OpenmodeE",
                "_ZNSt13basic_filebufIcSt11char_traitsIcEE6setbufEPci",
                "_ZNSt13basic_filebufIcSt11char_traitsIcEE4syncEv",
                "_ZNSt13basic_filebufIcSt11char_traitsIcEE5imbueERKSt6locale"}));
        v.push_back(streambuf_vtable(kFbW,
            {"_ZNSt13basic_filebufIwSt11char_traitsIwEED1Ev", "_ZNSt13basic_filebufIwSt11char_traitsIwEED0Ev",
                "_ZNSt13basic_filebufIwSt11char_traitsIwEE5_LockEv",
                "_ZNSt13basic_filebufIwSt11char_traitsIwEE7_UnlockEv",
                "_ZNSt13basic_filebufIwSt11char_traitsIwEE8overflowEi",
                "_ZNSt13basic_filebufIwSt11char_traitsIwEE9pbackfailEi",
                "_ZNSt15basic_streambufIwSt11char_traitsIwEE9showmanycEv",
                "_ZNSt13basic_filebufIwSt11char_traitsIwEE9underflowEv",
                "_ZNSt13basic_filebufIwSt11char_traitsIwEE5uflowEv",
                "_ZNSt15basic_streambufIwSt11char_traitsIwEE6xsgetnEPwi",
                "_ZNSt15basic_streambufIwSt11char_traitsIwEE6xsputnEPKwi",
                "_ZNSt13basic_filebufIwSt11char_traitsIwEE7seekoffElNSt5_IosbIiE8_SeekdirENS4_9_OpenmodeE",
                "_ZNSt13basic_filebufIwSt11char_traitsIwEE7seekposESt4fposISt9_MbstatetENSt5_IosbIiE9_OpenmodeE",
                "_ZNSt13basic_filebufIwSt11char_traitsIwEE6setbufEPwi",
                "_ZNSt13basic_filebufIwSt11char_traitsIwEE4syncEv",
                "_ZNSt13basic_filebufIwSt11char_traitsIwEE5imbueERKSt6locale"}));
        v.push_back(streambuf_vtable("St12strstreambuf",
            {"_ZNSt12strstreambufD1Ev", "_ZNSt12strstreambufD0Ev", "_ZNSt15basic_streambufIcSt11char_traitsIcEE5_LockEv",
                "_ZNSt15basic_streambufIcSt11char_traitsIcEE7_UnlockEv", "_ZNSt12strstreambuf8overflowEi",
                "_ZNSt12strstreambuf9pbackfailEi", "_ZNSt15basic_streambufIcSt11char_traitsIcEE9showmanycEv",
                "_ZNSt12strstreambuf9underflowEv", "_ZNSt15basic_streambufIcSt11char_traitsIcEE5uflowEv",
                "_ZNSt15basic_streambufIcSt11char_traitsIcEE6xsgetnEPci",
                "_ZNSt15basic_streambufIcSt11char_traitsIcEE6xsputnEPKci",
                "_ZNSt12strstreambuf7seekoffElNSt5_IosbIiE8_SeekdirENS1_9_OpenmodeE",
                "_ZNSt12strstreambuf7seekposESt4fposISt9_MbstatetENSt5_IosbIiE9_OpenmodeE",
                "_ZNSt15basic_streambufIcSt11char_traitsIcEE6setbufEPci",
                "_ZNSt15basic_streambufIcSt11char_traitsIcEE4syncEv",
                "_ZNSt15basic_streambufIcSt11char_traitsIcEE5imbueERKSt6locale"}));
        v.push_back(stream_vtable("Si", 8, "_ZNSiD1Ev", "_ZNSiD0Ev", "_ZTv0_n12_NSiD1Ev", "_ZTv0_n12_NSiD0Ev"));
        v.push_back(stream_vtable("So", 4, "_ZNSoD1Ev", "_ZNSoD0Ev", "_ZTv0_n12_NSoD1Ev", "_ZTv0_n12_NSoD0Ev"));
        v.push_back(stream_vtable(kIsW, 8, "_ZNSt13basic_istreamIwSt11char_traitsIwEED1Ev",
            "_ZNSt13basic_istreamIwSt11char_traitsIwEED0Ev", "_ZTv0_n12_NSt13basic_istreamIwSt11char_traitsIwEED1Ev",
            "_ZTv0_n12_NSt13basic_istreamIwSt11char_traitsIwEED0Ev"));
        v.push_back(stream_vtable(kOsW, 4, "_ZNSt13basic_ostreamIwSt11char_traitsIwEED1Ev",
            "_ZNSt13basic_ostreamIwSt11char_traitsIwEED0Ev", "_ZTv0_n12_NSt13basic_ostreamIwSt11char_traitsIwEED1Ev",
            "_ZTv0_n12_NSt13basic_ostreamIwSt11char_traitsIwEED0Ev"));
        v.push_back(iostream_vtable("Sd", 12, "_ZNSdD1Ev", "_ZNSdD0Ev", "_ZThn8_NSdD1Ev", "_ZThn8_NSdD0Ev",
            "_ZTv0_n12_NSdD1Ev", "_ZTv0_n12_NSdD0Ev"));
        v.push_back(stream_vtable("St10istrstream", 92, "_ZNSt10istrstreamD1Ev", "_ZNSt10istrstreamD0Ev",
            "_ZTv0_n12_NSt10istrstreamD1Ev", "_ZTv0_n12_NSt10istrstreamD0Ev"));
        v.push_back(stream_vtable("St10ostrstream", 88, "_ZNSt10ostrstreamD1Ev", "_ZNSt10ostrstreamD0Ev",
            "_ZTv0_n12_NSt10ostrstreamD1Ev", "_ZTv0_n12_NSt10ostrstreamD0Ev"));
        v.push_back(iostream_vtable("St9strstream", 96, "_ZNSt9strstreamD1Ev", "_ZNSt9strstreamD0Ev",
            "_ZThn8_NSt9strstreamD1Ev", "_ZThn8_NSt9strstreamD0Ev", "_ZTv0_n12_NSt9strstreamD1Ev",
            "_ZTv0_n12_NSt9strstreamD0Ev"));
        return v;
    }();
    return defs;
}

const std::vector<TypeDef>& type_defs()
{
    using K = TypeDef::Kind;
    // A virtual base's offset_flags: the offset of its virtual base offset in the vtable (-12) shifted by 8, virtual
    // and public; a non-virtual one's: its offset shifted by 8, public.
    constexpr s32 kVirtualPublic = static_cast<s32>(0xFFFFF403u);
    static const std::vector<TypeDef> defs = {
        {"St9exception", K::Class, 0, {}},
        {"St9bad_alloc", K::Si, 0, {{"St9exception", 0}}},
        {"St8bad_cast", K::Si, 0, {{"St9exception", 0}}},
        {"St11logic_error", K::Si, 0, {{"St9exception", 0}}},
        {"St12length_error", K::Si, 0, {{"St11logic_error", 0}}},
        {"St12out_of_range", K::Si, 0, {{"St11logic_error", 0}}},
        {"St13runtime_error", K::Si, 0, {{"St9exception", 0}}},
        {"NSt8ios_base7failureE", K::Si, 0, {{"St13runtime_error", 0}}},
        {"NSt6locale5facetE", K::Class, 0, {}},
        {"NSt6locale7_LocimpE", K::Si, 0, {{"NSt6locale5facetE", 0}}},
        {"St5_IosbIiE", K::Class, 0, {}},
        {"St8ios_base", K::Si, 0, {{"St5_IosbIiE", 0}}},
        {kIosC, K::Si, 0, {{"St8ios_base", 0}}},
        {kIosW, K::Si, 0, {{"St8ios_base", 0}}},
        {kSbC, K::Class, 0, {}},
        {kSbW, K::Class, 0, {}},
        {kFbC, K::Si, 0, {{kSbC, 0}}},
        {kFbW, K::Si, 0, {{kSbW, 0}}},
        {"St12strstreambuf", K::Si, 0, {{kSbC, 0}}},
        {"Si", K::Vmi, 0, {{kIosC, kVirtualPublic}}},
        {"So", K::Vmi, 0, {{kIosC, kVirtualPublic}}},
        {kIsW, K::Vmi, 0, {{kIosW, kVirtualPublic}}},
        {kOsW, K::Vmi, 0, {{kIosW, kVirtualPublic}}},
        {"Sd", K::Vmi, 2, {{"Si", 2}, {"So", (8 * 256) | 2}}},
        {"St10istrstream", K::Si, 0, {{"Si", 0}}},
        {"St10ostrstream", K::Si, 0, {{"So", 0}}},
        {"St9strstream", K::Si, 0, {{"Sd", 0}}},
    };
    return defs;
}

// ------------------------------------------------------------------------------------------------- for the facets

// The services the facets part calls here directly (stdcxx.hpp). `type` must be one of the library's constant type
// names (use_facet keeps its lazy facets by it).
u32 use_facet_of(PPUContext& ctx, u32 loc, std::string_view type) { return use_facet(ctx, loc, type); }
s32 streambuf_sgetc(PPUContext& ctx, u32 sb, u32 elem_size)
{
    return elem_size == 1 ? sgetc<1>(ctx, sb) : sgetc<4>(ctx, sb);
}
s32 streambuf_sbumpc(PPUContext& ctx, u32 sb, u32 elem_size)
{
    return elem_size == 1 ? sbumpc<1>(ctx, sb) : sbumpc<4>(ctx, sb);
}
s32 streambuf_sputc(PPUContext& ctx, u32 sb, u32 elem_size, u32 ch)
{
    return elem_size == 1 ? sputc<1>(ctx, sb, ch & 0xFF) : sputc<4>(ctx, sb, ch);
}

} // namespace stdcxx

using namespace stdcxx;

#define CXX(name, ...)                                                                                                   \
    ::pspocket::hle::Function{{}, name, ::pspocket::recomp::nid_of(name), &::pspocket::hle::detail::thunk<&__VA_ARGS__>, \
        false}
#define CXX_INTERNAL(name, ...)                                                                                          \
    ::pspocket::hle::Function{{}, name, ::pspocket::recomp::nid_of(name), &::pspocket::hle::detail::thunk<&__VA_ARGS__>, \
        true}
#define CXX_VAR(name, size) ::pspocket::hle::Variable{{}, name, ::pspocket::recomp::nid_of(name), size, 8, nullptr}

void register_stdcxx_streams(Registry& r)
{
    // The standard stream objects (laid out zeroed; ios_base::Init and _Winit make them).
    constexpr u32 kIstreamBytes = 60, kOstreamBytes = 56;

    r.add(kModule,
        {
            // operator new and delete
            CXX("_Znwj", op_new_), CXX("_ZnwjRKSt9nothrow_t", op_new_nothrow_), CXX("_Znwjj", op_new_aligned_),
            CXX("_ZnwjjRKSt9nothrow_t", op_new_aligned_nothrow_), CXX("_Znaj", op_new_),
            CXX("_ZnajRKSt9nothrow_t", op_new_nothrow_), CXX("_Znajj", op_new_aligned_),
            CXX("_ZnajjRKSt9nothrow_t", op_new_aligned_nothrow_), CXX("_ZdlPv", op_delete_),
            CXX("_ZdlPvRKSt9nothrow_t", op_delete2_), CXX("_ZdlPvS_", op_delete_placement_),
            CXX("_ZdlPvj", op_delete2_), CXX("_ZdlPvjRKSt9nothrow_t", op_delete3_),
            CXX("_ZdlPvjS_", op_delete_placement3_), CXX("_ZdaPv", op_delete_),
            CXX("_ZdaPvRKSt9nothrow_t", op_delete2_), CXX("_ZdaPvS_", op_delete_placement_),
            CXX("_ZdaPvj", op_delete2_), CXX("_ZdaPvjRKSt9nothrow_t", op_delete3_),
            CXX("_ZdaPvjS_", op_delete_placement3_),

            // exceptions
            CXX("_ZNKSt9exception4whatEv", exception_what_), CXX("_ZNKSt9exception6_RaiseEv", exception_raise_),
            CXX("_ZNKSt9exception8_DoraiseEv", exception_doraise_),
            CXX("_ZNSt9exception18_Set_raise_handlerEPFvRKS_E", set_raise_handler_),
            CXX("_ZNSt9exceptionD0Ev", exc_dtor_<kException, true>),
            CXX("_ZNSt9exceptionD1Ev", exc_dtor_<kException, false>),
            CXX("_ZNSt9exceptionD2Ev", exc_dtor_<kException, false>),
            CXX("_ZNKSt9bad_alloc4whatEv", bad_alloc_what_), CXX("_ZNKSt9bad_alloc8_DoraiseEv", doraise_),
            CXX("_ZNSt9bad_allocD0Ev", exc_dtor_<kBadAlloc, true>),
            CXX("_ZNSt9bad_allocD1Ev", exc_dtor_<kBadAlloc, false>),
            CXX("_ZNKSt8bad_cast4whatEv", bad_cast_what_), CXX("_ZNSt8bad_castC1Ev", bad_cast_ctor_),
            CXX("_ZNSt8bad_castD0Ev", exc_dtor_<kBadCast, true>),
            CXX("_ZNSt8bad_castD1Ev", exc_dtor_<kBadCast, false>),
            CXX("_ZNKSt11logic_error4whatEv", string_what_), CXX("_ZNKSt11logic_error8_DoraiseEv", doraise_),
            CXX("_ZNSt11logic_errorD0Ev", exc_dtor_<kLogicError, true>),
            CXX("_ZNSt11logic_errorD1Ev", exc_dtor_<kLogicError, false>),
            CXX("_ZNSt11logic_errorD2Ev", exc_dtor_<kLogicError, false>),
            CXX("_ZNKSt12length_error8_DoraiseEv", doraise_),
            CXX("_ZNSt12length_errorD0Ev", exc_dtor_<kLengthError, true>),
            CXX("_ZNSt12length_errorD1Ev", exc_dtor_<kLengthError, false>),
            CXX("_ZNKSt12out_of_range8_DoraiseEv", doraise_),
            CXX("_ZNSt12out_of_rangeD0Ev", exc_dtor_<kOutOfRange, true>),
            CXX("_ZNSt12out_of_rangeD1Ev", exc_dtor_<kOutOfRange, false>),
            CXX("_ZNKSt13runtime_error4whatEv", string_what_), CXX("_ZNKSt13runtime_error8_DoraiseEv", doraise_),
            CXX("_ZNSt13runtime_errorD0Ev", exc_dtor_<kRuntimeError, true>),
            CXX("_ZNSt13runtime_errorD1Ev", exc_dtor_<kRuntimeError, false>),
            CXX("_ZNKSt8ios_base7failure8_DoraiseEv", doraise_),
            CXX("_ZNSt8ios_base7failureD0Ev", failure_dtor_<true>),
            CXX("_ZNSt8ios_base7failureD1Ev", failure_dtor_<false>), CXX("_ZSt6_ThrowRKSt9exception", _Throw_),
            CXX("_ZNKSt12_String_base5_XlenEv", xlen_), CXX("_ZNKSt12_String_base5_XranEv", xran_),

            // handlers
            CXX("_ZSt9terminatev", terminate_), CXX("_ZSt10unexpectedv", unexpected_),
            CXX("_ZSt13set_terminatePFvvE", set_terminate_), CXX("_ZSt14set_unexpectedPFvvE", set_unexpected_),
            CXX("_ZSt15set_new_handlerPFvvE", set_new_handler_), CXX("_ZSt14_Debug_messagePKcS0_", _Debug_message_),
            CXX_INTERNAL("_pspocket_stdcxx_default_terminate", default_terminate_),
            CXX_INTERNAL("_pspocket_stdcxx_default_unexpected", default_unexpected_),

            // _Mutex
            CXX("_ZNSt6_MutexC1Ev", mutex_ctor_), CXX("_ZNSt6_MutexC2Ev", mutex_ctor_),
            CXX("_ZNSt6_MutexD1Ev", mutex_dtor_), CXX("_ZNSt6_MutexD2Ev", mutex_dtor_),
            CXX("_ZNSt6_Mutex5_LockEv", mutex_lock_), CXX("_ZNSt6_Mutex7_UnlockEv", mutex_unlock_),

            // basic_string<char>
            CXX("_ZNSsC1Ev", str_ctor_<1>), CXX("_ZNSsC1EPKc", str_ctor_cstr_), CXX("_ZNSsC1ERKSs", str_ctor_copy_),
            CXX("_ZNSsD1Ev", str_dtor_), CXX("_ZNSs5_GrowEjb", str_grow_<1>), CXX("_ZNSs5_TidyEbj", str_tidy_<1>),
            CXX("_ZNSs5eraseEjj", str_erase_<1>), CXX("_ZNSs6appendEjc", str_append_<1>),
            CXX("_ZNSs6assignERKSsjj", str_assign_<1>), CXX("_ZNSs6insertEjjc", str_insert_<1>),
            CXX("_ZNSs7replaceEjjPKcj", str_replace_<1>),
            // basic_string<wchar_t>
            CXX("_ZNSbIwSt11char_traitsIwESaIwEEC1Ev", str_ctor_<4>),
            CXX("_ZNSbIwSt11char_traitsIwESaIwEE5_GrowEjb", str_grow_<4>),
            CXX("_ZNSbIwSt11char_traitsIwESaIwEE5_TidyEbj", str_tidy_<4>),
            CXX("_ZNSbIwSt11char_traitsIwESaIwEE5eraseEjj", str_erase_<4>),
            CXX("_ZNSbIwSt11char_traitsIwESaIwEE6appendEjw", str_append_<4>),
            CXX("_ZNSbIwSt11char_traitsIwESaIwEE6assignERKS2_jj", str_assign_<4>),
            CXX("_ZNSbIwSt11char_traitsIwESaIwEE6insertEjjw", str_insert_<4>),
            CXX("_ZNSbIwSt11char_traitsIwESaIwEE7replaceEjjPKwj", str_replace_<4>),

            // locale::facet, locale::id
            CXX("_ZNSt6locale5facet7_IncrefEv", facet_incref_), CXX("_ZNSt6locale5facet7_DecrefEv", facet_decref_),
            CXX("_ZNSt6locale5facet9_RegisterEv", facet_register_),
            CXX("_ZNSt6locale5facetD0Ev", facet_dtor_<true>), CXX("_ZNSt6locale5facetD1Ev", facet_dtor_<false>),
            CXX("_ZNSt6locale5facetD2Ev", facet_dtor_<false>), CXX("_ZNSt6locale2idcvjEv", id_value_),
            CXX("_Fac_tidy", fac_tidy_),
            // locale
            CXX("_ZNSt6locale5_InitEv", locale_init_), CXX("_ZNSt6locale7classicEv", classic_),
            CXX("_ZNSt6locale5emptyEv", empty_), CXX("_ZNSt6locale6globalERKS_", global_),
            CXX("_ZNKSt6locale9_GetfacetEj", getfacet_), CXX("_ZNKSt6localeeqERKS_", locale_eq_),
            CXX("_ZNSt6locale7_AddfacEPNS_5facetEjj", locale_addfac_), CXX("_ZNSt6localeC1Ev", locale_ctor_),
            CXX("_ZNSt6localeC2Ev", locale_ctor_), CXX("_ZNSt6localeC1EPKci", locale_ctor_name_),
            CXX("_ZNSt6localeC2EPKci", locale_ctor_name_), CXX("_ZNSt6localeC1ERKS_PKci", locale_ctor_loc_name_),
            CXX("_ZNSt6localeC2ERKS_PKci", locale_ctor_loc_name_), CXX("_ZNSt6localeC1ERKS_S1_i", locale_ctor_loc_loc_),
            CXX("_ZNSt6localeC2ERKS_S1_i", locale_ctor_loc_loc_), CXX("_ZNSt6localeD1Ev", locale_dtor_),
            CXX("_Deletegloballocale", deletegloballocale_), CXX("__Setgloballocale", setgloballocale_),
            // locale::_Locimp
            CXX("_ZNSt6locale7_LocimpC1Eb", imp_ctor_), CXX("_ZNSt6locale7_LocimpC2Eb", imp_ctor_),
            CXX("_ZNSt6locale7_LocimpC1ERKS0_", imp_copy_ctor_), CXX("_ZNSt6locale7_LocimpC2ERKS0_", imp_copy_ctor_),
            CXX("_ZNSt6locale7_LocimpD0Ev", imp_dtor_<true>), CXX("_ZNSt6locale7_LocimpD1Ev", imp_dtor_<false>),
            CXX("_ZNSt6locale7_LocimpD2Ev", imp_dtor_<false>),
            CXX("_ZNSt6locale7_Locimp7_AddfacEPNS_5facetEj", imp_addfac_),
            CXX("_ZNSt6locale7_Locimp8_MakelocERKSt8_LocinfoiPS0_PKS_", makeloc_),
            CXX("_ZNSt6locale7_Locimp9_MakexlocERKSt8_LocinfoiPS0_PKS_", makexloc_),
            CXX("_ZNSt6locale7_Locimp9_MakewlocERKSt8_LocinfoiPS0_PKS_", makewloc_),
            // _Locinfo
            CXX("_ZNSt8_LocinfoC1EPKc", info_ctor_name_), CXX("_ZNSt8_LocinfoC2EPKc", info_ctor_name_),
            CXX("_ZNSt8_LocinfoC1EiPKc", info_ctor_cat_), CXX("_ZNSt8_LocinfoC2EiPKc", info_ctor_cat_),
            CXX("_ZNSt8_LocinfoD1Ev", info_dtor_), CXX("_ZNSt8_LocinfoD2Ev", info_dtor_),
            CXX("_ZNSt8_Locinfo8_AddcatsEiPKc", info_addcats_), CXX("_ZNKSt8_Locinfo7_GetcvtEv", info_getcvt_),
            CXX("_ZNKSt8_Locinfo8_GetcollEv", info_getcoll_), CXX("_ZNKSt8_Locinfo9_GetctypeEv", info_getctype_),
            // use_facet
            CXX("_ZSt9use_facetISt10moneypunctIcLb0EEERKT_RKSt6locale", use_facet_<kMoneypunctC0>),
            CXX("_ZSt9use_facetISt10moneypunctIcLb1EEERKT_RKSt6locale", use_facet_<kMoneypunctC1>),
            CXX("_ZSt9use_facetISt10moneypunctIwLb0EEERKT_RKSt6locale", use_facet_<kMoneypunctW0>),
            CXX("_ZSt9use_facetISt10moneypunctIwLb1EEERKT_RKSt6locale", use_facet_<kMoneypunctW1>),
            CXX("_ZSt9use_facetISt5ctypeIwEERKT_RKSt6locale", use_facet_<kCtypeW>),
            CXX("_ZSt9use_facetISt8numpunctIcEERKT_RKSt6locale", use_facet_<kNumpunctC>),
            CXX("_ZSt9use_facetISt8numpunctIwEERKT_RKSt6locale", use_facet_<kNumpunctW>),

            // ios_base
            CXX("_ZNSt8ios_base17register_callbackEPFvNS_5eventERS_iEi", ios_register_callback_),
            CXX("_ZNSt8ios_base5_InitEv", ios_init_), CXX("_ZNSt8ios_base5_TidyEv", ios_tidy_),
            CXX("_ZNSt8ios_base5clearENSt5_IosbIiE8_IostateEb", ios_clear_),
            CXX("_ZNSt8ios_base5imbueERKSt6locale", ios_imbue_), CXX("_ZNSt8ios_base7_AddstdEv", ios_addstd_),
            CXX("_ZNSt8ios_base7copyfmtERKS_", ios_copyfmt_), CXX("_ZNSt8ios_base8_CallfnsENS_5eventE", ios_callfns_),
            CXX("_ZNSt8ios_base8_FindarrEi", ios_findarr_), CXX("_ZNSt8ios_baseD0Ev", ios_base_dtor_<true>),
            CXX("_ZNSt8ios_baseD1Ev", ios_base_dtor_<false>), CXX("_ZNSt8ios_baseD2Ev", ios_base_dtor_<false>),
            CXX("_ZNSt8ios_base4InitC1Ev", std_init_<1>), CXX("_ZNSt8ios_base4InitC2Ev", std_init_<1>),
            CXX("_ZNSt8ios_base4InitD1Ev", std_fini_<1>), CXX("_ZNSt8ios_base4InitD2Ev", std_fini_<1>),
            CXX("_ZNSt6_WinitC1Ev", std_init_<4>), CXX("_ZNSt6_WinitC2Ev", std_init_<4>),
            CXX("_ZNSt6_WinitD1Ev", std_fini_<4>), CXX("_ZNSt6_WinitD2Ev", std_fini_<4>),

            // basic_ios
            CXX("_ZNSt9basic_iosIcSt11char_traitsIcEE4initEPSt15basic_streambufIcS1_Eb", basic_ios_init_<1>),
            CXX("_ZNSt9basic_iosIcSt11char_traitsIcEED0Ev", basic_ios_dtor_<1, true>),
            CXX("_ZNSt9basic_iosIcSt11char_traitsIcEED1Ev", basic_ios_dtor_<1, false>),
            CXX("_ZNSt9basic_iosIwSt11char_traitsIwEE4initEPSt15basic_streambufIwS1_Eb", basic_ios_init_<4>),
            CXX("_ZNSt9basic_iosIwSt11char_traitsIwEED0Ev", basic_ios_dtor_<4, true>),
            CXX("_ZNSt9basic_iosIwSt11char_traitsIwEED1Ev", basic_ios_dtor_<4, false>),

            // basic_streambuf<char>
            CXX("_ZNSt15basic_streambufIcSt11char_traitsIcEED0Ev", sb_dtor_<1, true>),
            CXX("_ZNSt15basic_streambufIcSt11char_traitsIcEED1Ev", sb_dtor_<1, false>),
            CXX("_ZNSt15basic_streambufIcSt11char_traitsIcEE5_LockEv", sb_lock_<1>),
            CXX("_ZNSt15basic_streambufIcSt11char_traitsIcEE7_UnlockEv", sb_unlock_<1>),
            CXX("_ZNSt15basic_streambufIcSt11char_traitsIcEE8overflowEi", sb_eof_<1>),
            CXX("_ZNSt15basic_streambufIcSt11char_traitsIcEE9pbackfailEi", sb_eof_<1>),
            CXX("_ZNSt15basic_streambufIcSt11char_traitsIcEE9showmanycEv", sb_showmanyc_<1>),
            CXX("_ZNSt15basic_streambufIcSt11char_traitsIcEE9underflowEv", sb_underflow_<1>),
            CXX("_ZNSt15basic_streambufIcSt11char_traitsIcEE5uflowEv", sb_uflow_<1>),
            CXX("_ZNSt15basic_streambufIcSt11char_traitsIcEE6xsgetnEPci", sb_xsgetn_<1>),
            CXX("_ZNSt15basic_streambufIcSt11char_traitsIcEE6xsputnEPKci", sb_xsputn_<1>),
            CXX("_ZNSt15basic_streambufIcSt11char_traitsIcEE7seekoffElNSt5_IosbIiE8_SeekdirENS4_9_OpenmodeE",
                sb_seekoff_<1>),
            CXX("_ZNSt15basic_streambufIcSt11char_traitsIcEE7seekposESt4fposISt9_MbstatetENSt5_IosbIiE9_OpenmodeE",
                sb_seekpos_<1>),
            CXX("_ZNSt15basic_streambufIcSt11char_traitsIcEE6setbufEPci", sb_setbuf_<1>),
            CXX("_ZNSt15basic_streambufIcSt11char_traitsIcEE4syncEv", sb_sync_<1>),
            CXX("_ZNSt15basic_streambufIcSt11char_traitsIcEE5imbueERKSt6locale", sb_imbue_<1>),
            // basic_streambuf<wchar_t>
            CXX("_ZNSt15basic_streambufIwSt11char_traitsIwEED0Ev", sb_dtor_<4, true>),
            CXX("_ZNSt15basic_streambufIwSt11char_traitsIwEED1Ev", sb_dtor_<4, false>),
            CXX("_ZNSt15basic_streambufIwSt11char_traitsIwEE5_LockEv", sb_lock_<4>),
            CXX("_ZNSt15basic_streambufIwSt11char_traitsIwEE7_UnlockEv", sb_unlock_<4>),
            CXX("_ZNSt15basic_streambufIwSt11char_traitsIwEE8overflowEi", sb_eof_<4>),
            CXX("_ZNSt15basic_streambufIwSt11char_traitsIwEE9pbackfailEi", sb_eof_<4>),
            CXX("_ZNSt15basic_streambufIwSt11char_traitsIwEE9showmanycEv", sb_showmanyc_<4>),
            CXX("_ZNSt15basic_streambufIwSt11char_traitsIwEE9underflowEv", sb_underflow_<4>),
            CXX("_ZNSt15basic_streambufIwSt11char_traitsIwEE5uflowEv", sb_uflow_<4>),
            CXX("_ZNSt15basic_streambufIwSt11char_traitsIwEE6xsgetnEPwi", sb_xsgetn_<4>),
            CXX("_ZNSt15basic_streambufIwSt11char_traitsIwEE6xsputnEPKwi", sb_xsputn_<4>),
            CXX("_ZNSt15basic_streambufIwSt11char_traitsIwEE7seekoffElNSt5_IosbIiE8_SeekdirENS4_9_OpenmodeE",
                sb_seekoff_<4>),
            CXX("_ZNSt15basic_streambufIwSt11char_traitsIwEE7seekposESt4fposISt9_MbstatetENSt5_IosbIiE9_OpenmodeE",
                sb_seekpos_<4>),
            CXX("_ZNSt15basic_streambufIwSt11char_traitsIwEE6setbufEPwi", sb_setbuf_<4>),
            CXX("_ZNSt15basic_streambufIwSt11char_traitsIwEE4syncEv", sb_sync_<4>),
            CXX("_ZNSt15basic_streambufIwSt11char_traitsIwEE5imbueERKSt6locale", sb_imbue_<4>),

            // basic_filebuf<char>
            CXX("_ZNSt13basic_filebufIcSt11char_traitsIcEED0Ev", filebuf_dtor_<1, true>),
            CXX("_ZNSt13basic_filebufIcSt11char_traitsIcEED1Ev", filebuf_dtor_<1, false>),
            CXX("_ZNSt13basic_filebufIcSt11char_traitsIcEE5_InitEPSt6_FiletNS2_7_InitflE", filebuf_init_<1>),
            CXX("_ZNSt13basic_filebufIcSt11char_traitsIcEE5_LockEv", filebuf_lock_<1>),
            CXX("_ZNSt13basic_filebufIcSt11char_traitsIcEE7_UnlockEv", filebuf_unlock_<1>),
            CXX("_ZNSt13basic_filebufIcSt11char_traitsIcEE5imbueERKSt6locale", filebuf_imbue_<1>),
            CXX("_ZNSt13basic_filebufIcSt11char_traitsIcEE8overflowEi", filebuf_overflow_<1>),
            CXX("_ZNSt13basic_filebufIcSt11char_traitsIcEE9pbackfailEi", filebuf_pbackfail_<1>),
            CXX("_ZNSt13basic_filebufIcSt11char_traitsIcEE9underflowEv", filebuf_underflow_<1>),
            CXX("_ZNSt13basic_filebufIcSt11char_traitsIcEE5uflowEv", filebuf_uflow_<1>),
            CXX("_ZNSt13basic_filebufIcSt11char_traitsIcEE7seekoffElNSt5_IosbIiE8_SeekdirENS4_9_OpenmodeE",
                filebuf_seekoff_<1>),
            CXX("_ZNSt13basic_filebufIcSt11char_traitsIcEE7seekposESt4fposISt9_MbstatetENSt5_IosbIiE9_OpenmodeE",
                filebuf_seekpos_<1>),
            CXX("_ZNSt13basic_filebufIcSt11char_traitsIcEE6setbufEPci", filebuf_setbuf_<1>),
            CXX("_ZNSt13basic_filebufIcSt11char_traitsIcEE4syncEv", filebuf_sync_<1>),
            CXX("_ZNSt13basic_filebufIcSt11char_traitsIcEE9_EndwriteEv", filebuf_endwrite_<1>),
            // basic_filebuf<wchar_t>
            CXX("_ZNSt13basic_filebufIwSt11char_traitsIwEED0Ev", filebuf_dtor_<4, true>),
            CXX("_ZNSt13basic_filebufIwSt11char_traitsIwEED1Ev", filebuf_dtor_<4, false>),
            CXX("_ZNSt13basic_filebufIwSt11char_traitsIwEE5_InitEPSt6_FiletNS2_7_InitflE", filebuf_init_<4>),
            CXX("_ZNSt13basic_filebufIwSt11char_traitsIwEE5_LockEv", filebuf_lock_<4>),
            CXX("_ZNSt13basic_filebufIwSt11char_traitsIwEE7_UnlockEv", filebuf_unlock_<4>),
            CXX("_ZNSt13basic_filebufIwSt11char_traitsIwEE5imbueERKSt6locale", filebuf_imbue_<4>),
            CXX("_ZNSt13basic_filebufIwSt11char_traitsIwEE8overflowEi", filebuf_overflow_<4>),
            CXX("_ZNSt13basic_filebufIwSt11char_traitsIwEE9pbackfailEi", filebuf_pbackfail_<4>),
            CXX("_ZNSt13basic_filebufIwSt11char_traitsIwEE9underflowEv", filebuf_underflow_<4>),
            CXX("_ZNSt13basic_filebufIwSt11char_traitsIwEE5uflowEv", filebuf_uflow_<4>),
            CXX("_ZNSt13basic_filebufIwSt11char_traitsIwEE7seekoffElNSt5_IosbIiE8_SeekdirENS4_9_OpenmodeE",
                filebuf_seekoff_<4>),
            CXX("_ZNSt13basic_filebufIwSt11char_traitsIwEE7seekposESt4fposISt9_MbstatetENSt5_IosbIiE9_OpenmodeE",
                filebuf_seekpos_<4>),
            CXX("_ZNSt13basic_filebufIwSt11char_traitsIwEE6setbufEPwi", filebuf_setbuf_<4>),
            CXX("_ZNSt13basic_filebufIwSt11char_traitsIwEE4syncEv", filebuf_sync_<4>),
            CXX("_ZNSt13basic_filebufIwSt11char_traitsIwEE9_EndwriteEv", filebuf_endwrite_<4>),
            CXX("_ZSt7_FiopenPKcNSt5_IosbIiE9_OpenmodeEi", fiopen_),
            CXX("_ZSt7_FiopenPKwNSt5_IosbIiE9_OpenmodeEi", wfiopen_),

            // istream, ostream, iostream
            CXX("_ZNSiD0Ev", stream_dtor_<kIstreamC, true>), CXX("_ZNSiD1Ev", stream_dtor_<kIstreamC, false>),
            CXX("_ZNSoD0Ev", stream_dtor_<kOstreamC, true>), CXX("_ZNSoD1Ev", stream_dtor_<kOstreamC, false>),
            CXX("_ZNSdD0Ev", stream_dtor_<kIostreamC, true>), CXX("_ZNSdD1Ev", stream_dtor_<kIostreamC, false>),
            CXX("_ZNSt13basic_istreamIwSt11char_traitsIwEED0Ev", stream_dtor_<kIstreamW, true>),
            CXX("_ZNSt13basic_istreamIwSt11char_traitsIwEED1Ev", stream_dtor_<kIstreamW, false>),
            CXX("_ZNSt13basic_ostreamIwSt11char_traitsIwEED0Ev", stream_dtor_<kOstreamW, true>),
            CXX("_ZNSt13basic_ostreamIwSt11char_traitsIwEED1Ev", stream_dtor_<kOstreamW, false>),
            CXX("_ZTv0_n12_NSiD0Ev", vthunk_<stream_dtor_<kIstreamC, true>>),
            CXX("_ZTv0_n12_NSiD1Ev", vthunk_<stream_dtor_<kIstreamC, false>>),
            CXX("_ZTv0_n12_NSoD0Ev", vthunk_<stream_dtor_<kOstreamC, true>>),
            CXX("_ZTv0_n12_NSoD1Ev", vthunk_<stream_dtor_<kOstreamC, false>>),
            CXX("_ZTv0_n12_NSdD0Ev", vthunk_<stream_dtor_<kIostreamC, true>>),
            CXX("_ZTv0_n12_NSdD1Ev", vthunk_<stream_dtor_<kIostreamC, false>>),
            CXX("_ZThn8_NSdD0Ev", nvthunk8_<stream_dtor_<kIostreamC, true>>),
            CXX("_ZThn8_NSdD1Ev", nvthunk8_<stream_dtor_<kIostreamC, false>>),
            CXX("_ZTv0_n12_NSt13basic_istreamIwSt11char_traitsIwEED0Ev", vthunk_<stream_dtor_<kIstreamW, true>>),
            CXX("_ZTv0_n12_NSt13basic_istreamIwSt11char_traitsIwEED1Ev", vthunk_<stream_dtor_<kIstreamW, false>>),
            CXX("_ZTv0_n12_NSt13basic_ostreamIwSt11char_traitsIwEED0Ev", vthunk_<stream_dtor_<kOstreamW, true>>),
            CXX("_ZTv0_n12_NSt13basic_ostreamIwSt11char_traitsIwEED1Ev", vthunk_<stream_dtor_<kOstreamW, false>>),

            // strstreambuf
            CXX("_ZNSt12strstreambuf5_InitEiPcS0_i", ssb_init_), CXX("_ZNSt12strstreambuf5_TidyEv", ssb_tidy_),
            CXX("_ZNSt12strstreambuf6freezeEb", ssb_freeze_), CXX("_ZNSt12strstreambuf8overflowEi", ssb_overflow_),
            CXX("_ZNSt12strstreambuf9pbackfailEi", ssb_pbackfail_),
            CXX("_ZNSt12strstreambuf9underflowEv", ssb_underflow_),
            CXX("_ZNSt12strstreambuf7seekoffElNSt5_IosbIiE8_SeekdirENS1_9_OpenmodeE", ssb_seekoff_),
            CXX("_ZNSt12strstreambuf7seekposESt4fposISt9_MbstatetENSt5_IosbIiE9_OpenmodeE", ssb_seekpos_),
            CXX("_ZNSt12strstreambufD0Ev", ssb_dtor_<true>), CXX("_ZNSt12strstreambufD1Ev", ssb_dtor_<false>),
            CXX("_ZNSt12strstreambufD2Ev", ssb_dtor_<false>),
            // istrstream, ostrstream, strstream
            CXX("_ZNSt10istrstreamD0Ev", stream_dtor_<kIstrstream, true>),
            CXX("_ZNSt10istrstreamD1Ev", stream_dtor_<kIstrstream, false>),
            CXX("_ZNSt10istrstreamD2Ev", stream_dtor_base_<kIstrstream>),
            CXX("_ZTv0_n12_NSt10istrstreamD0Ev", vthunk_<stream_dtor_<kIstrstream, true>>),
            CXX("_ZTv0_n12_NSt10istrstreamD1Ev", vthunk_<stream_dtor_<kIstrstream, false>>),
            CXX("_ZNSt10ostrstreamC1EPciNSt5_IosbIiE9_OpenmodeE", strstream_ctor_<kOstrstream>),
            CXX("_ZNSt10ostrstreamC2EPciNSt5_IosbIiE9_OpenmodeE", strstream_ctor_base_<kOstrstream>),
            CXX("_ZNSt10ostrstreamD0Ev", stream_dtor_<kOstrstream, true>),
            CXX("_ZNSt10ostrstreamD1Ev", stream_dtor_<kOstrstream, false>),
            CXX("_ZNSt10ostrstreamD2Ev", stream_dtor_base_<kOstrstream>),
            CXX("_ZTv0_n12_NSt10ostrstreamD0Ev", vthunk_<stream_dtor_<kOstrstream, true>>),
            CXX("_ZTv0_n12_NSt10ostrstreamD1Ev", vthunk_<stream_dtor_<kOstrstream, false>>),
            CXX("_ZNSt9strstreamC1EPciNSt5_IosbIiE9_OpenmodeE", strstream_ctor_<kStrstream>),
            CXX("_ZNSt9strstreamC2EPciNSt5_IosbIiE9_OpenmodeE", strstream_ctor_base_<kStrstream>),
            CXX("_ZNSt9strstreamD0Ev", stream_dtor_<kStrstream, true>),
            CXX("_ZNSt9strstreamD1Ev", stream_dtor_<kStrstream, false>),
            CXX("_ZNSt9strstreamD2Ev", stream_dtor_base_<kStrstream>),
            CXX("_ZThn8_NSt9strstreamD0Ev", nvthunk8_<stream_dtor_<kStrstream, true>>),
            CXX("_ZThn8_NSt9strstreamD1Ev", nvthunk8_<stream_dtor_<kStrstream, false>>),
            CXX("_ZTv0_n12_NSt9strstreamD0Ev", vthunk_<stream_dtor_<kStrstream, true>>),
            CXX("_ZTv0_n12_NSt9strstreamD1Ev", vthunk_<stream_dtor_<kStrstream, false>>),

            // the stream buffer iterators
            CXX("_ZNKSt19istreambuf_iteratorIwSt11char_traitsIwEEdeEv", it_deref_<4>),
            CXX("_ZNSt19istreambuf_iteratorIcSt11char_traitsIcEE5_PeekEv", it_peek_<1>),
            CXX("_ZNSt19istreambuf_iteratorIwSt11char_traitsIwEE5_PeekEv", it_peek_<4>),
            CXX("_ZNSt19istreambuf_iteratorIwSt11char_traitsIwEE4_IncEv", it_inc_<4>),
            CXX("_ZNSt19ostreambuf_iteratorIwSt11char_traitsIwEEaSEw", oit_assign_<4>),

            // the manipulators
            CXX("_ZSt4setwi", setw_), CXX("_ZSt12setprecisioni", setprecision_), CXX("_ZSt7setbasei", setbase_),
            CXX("_ZSt11setiosflagsNSt5_IosbIiE9_FmtflagsE", setiosflags_),
            CXX("_ZSt13resetiosflagsNSt5_IosbIiE9_FmtflagsE", resetiosflags_),
            CXX_INTERNAL("_pspocket_stdcxx_swfun", swfun_), CXX_INTERNAL("_pspocket_stdcxx_spfun", spfun_),
            CXX_INTERNAL("_pspocket_stdcxx_sbfun", sbfun_), CXX_INTERNAL("_pspocket_stdcxx_sifun", sifun_),
            CXX_INTERNAL("_pspocket_stdcxx_rsfun", rsfun_),
        });

    r.add_variables(kModule,
        {
            CXX_VAR("_ZSt3cin", kIstreamBytes), CXX_VAR("_ZSt4cout", kOstreamBytes),
            CXX_VAR("_ZSt4cerr", kOstreamBytes), CXX_VAR("_ZSt4clog", kOstreamBytes),
            CXX_VAR("_ZSt4wcin", kIstreamBytes), CXX_VAR("_ZSt5wcout", kOstreamBytes),
            CXX_VAR("_ZSt5wcerr", kOstreamBytes), CXX_VAR("_ZSt5wclog", kOstreamBytes),
        });
}

} // namespace pspocket::hle

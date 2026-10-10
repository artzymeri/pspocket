// sys_libstdcxx, the facets part (D-561): ctype, codecvt, collate, messages, numpunct, num_get, num_put, moneypunct
// (_Mpunct), money_get, money_put, time_get and time_put for char and wchar_t, their bases' destructors and helpers
// (stdcxx_facets*.hpp). This file has the classes' vtables and type_info and registers the functions; the streams part
// (stdcxx_streams.cpp, D-560) makes a facet through its type's _Getcat, by mangled name in the registry, and the facets
// call it back for use_facet and the stream buffers (stdcxx.hpp).

#include "stdcxx_facets_time.hpp"

namespace pspocket::hle {

namespace stdcxx {

using namespace facets;

namespace {

// A facet's vtable: [~D1, ~D0, its virtual functions in the header's order].
VtableDef facet_vtable(std::string_view cls, std::initializer_list<std::string_view> fns)
{
    VtableDef d{cls, {V(0), T(cls)}, {2}};
    for (const std::string_view f : fns) d.words.push_back(F(f));
    return d;
}

// The mangled names, by class.
#define CT_C "_ZNKSt5ctypeIcE"
#define CT_W "_ZNKSt5ctypeIwE"
#define CVT_B "_ZNKSt12codecvt_base"
#define CVT_C "_ZNKSt7codecvtIccSt9_MbstatetE"
#define CVT_W "_ZNKSt7codecvtIwcSt9_MbstatetE"
#define COLL_C "_ZNKSt7collateIcE"
#define COLL_W "_ZNKSt7collateIwE"
#define MSG_C "_ZNKSt8messagesIcE"
#define MSG_W "_ZNKSt8messagesIwE"
#define NP_C "_ZNKSt8numpunctIcE"
#define NP_W "_ZNKSt8numpunctIwE"
#define NG_C "_ZNKSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE"
#define NG_W "_ZNKSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE"
#define NG_GET "6do_getES3_S3_RSt8ios_baseRNSt5_IosbIiE8_IostateER"
#define NPUT_C "_ZNKSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE"
#define NPUT_W "_ZNKSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE"
#define NPUT_PUT "6do_putES3_RSt8ios_base"
#define MP_C "_ZNKSt7_MpunctIcE"
#define MP_W "_ZNKSt7_MpunctIwE"
#define MG_C "_ZNKSt9money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE"
#define MG_W "_ZNKSt9money_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE"
#define MG_GET "6do_getES3_S3_bRSt8ios_baseRNSt5_IosbIiE8_IostateER"
#define MPUT_C "_ZNKSt9money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE"
#define MPUT_W "_ZNKSt9money_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE"
#define TG_C "_ZNKSt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE"
#define TG_W "_ZNKSt8time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE"
#define TG_ARGS "ES3_S3_RSt8ios_baseRNSt5_IosbIiE8_IostateEPSt2tm"
#define TPUT_C "_ZNKSt8time_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE"
#define TPUT_W "_ZNKSt8time_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE"

// Classes' own (non-const) members: destructors, _Getcat, _Init, constructors.
#define C_CT_C "_ZNSt5ctypeIcE"
#define C_CT_W "_ZNSt5ctypeIwE"
#define C_CVT_C "_ZNSt7codecvtIccSt9_MbstatetE"
#define C_CVT_W "_ZNSt7codecvtIwcSt9_MbstatetE"
#define C_COLL_C "_ZNSt7collateIcE"
#define C_COLL_W "_ZNSt7collateIwE"
#define C_MSG_C "_ZNSt8messagesIcE"
#define C_MSG_W "_ZNSt8messagesIwE"
#define C_NP_C "_ZNSt8numpunctIcE"
#define C_NP_W "_ZNSt8numpunctIwE"
#define C_NG_C "_ZNSt7num_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE"
#define C_NG_W "_ZNSt7num_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE"
#define C_NPUT_C "_ZNSt7num_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE"
#define C_NPUT_W "_ZNSt7num_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE"
#define C_MP_C "_ZNSt7_MpunctIcE"
#define C_MP_W "_ZNSt7_MpunctIwE"
#define C_MPC0 "_ZNSt10moneypunctIcLb0EE"
#define C_MPC1 "_ZNSt10moneypunctIcLb1EE"
#define C_MPW0 "_ZNSt10moneypunctIwLb0EE"
#define C_MPW1 "_ZNSt10moneypunctIwLb1EE"
#define C_MG_C "_ZNSt9money_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE"
#define C_MG_W "_ZNSt9money_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE"
#define C_MPUT_C "_ZNSt9money_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE"
#define C_MPUT_W "_ZNSt9money_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE"
#define C_TG_C "_ZNSt8time_getIcSt19istreambuf_iteratorIcSt11char_traitsIcEEE"
#define C_TG_W "_ZNSt8time_getIwSt19istreambuf_iteratorIwSt11char_traitsIwEEE"
#define C_TPUT_C "_ZNSt8time_putIcSt19ostreambuf_iteratorIcSt11char_traitsIcEEE"
#define C_TPUT_W "_ZNSt8time_putIwSt19ostreambuf_iteratorIwSt11char_traitsIwEEE"
#define GETCAT "7_GetcatEPPKNSt6locale5facetE"

} // namespace

const std::vector<VtableDef>& facet_vtable_defs()
{
    static const std::vector<VtableDef> defs = [] {
        std::vector<VtableDef> v;
        v.push_back(facet_vtable(kCtypeBase, {"_ZNSt10ctype_baseD1Ev", "_ZNSt10ctype_baseD0Ev"}));
        v.push_back(facet_vtable(kCtypeC,
            {C_CT_C "D1Ev", C_CT_C "D0Ev", CT_C "10do_tolowerEc", CT_C "10do_tolowerEPcPKc", CT_C "10do_toupperEc",
                CT_C "10do_toupperEPcPKc", CT_C "8do_widenEc", CT_C "8do_widenEPKcS2_Pc", CT_C "9do_narrowEcc",
                CT_C "9do_narrowEPKcS2_cPc"}));
        v.push_back(facet_vtable(kCtypeW,
            {C_CT_W "D1Ev", C_CT_W "D0Ev", CT_W "5do_isEsw", CT_W "5do_isEPKwS2_Ps", CT_W "10do_scan_isEsPKwS2_",
                CT_W "11do_scan_notEsPKwS2_", CT_W "10do_tolowerEw", CT_W "10do_tolowerEPwPKw", CT_W "10do_toupperEw",
                CT_W "10do_toupperEPwPKw", CT_W "8do_widenEc", CT_W "8do_widenEPKcS2_Pw", CT_W "9do_narrowEwc",
                CT_W "9do_narrowEPKwS2_cPc"}));
        v.push_back(facet_vtable(kCvtBase,
            {"_ZNSt12codecvt_baseD1Ev", "_ZNSt12codecvt_baseD0Ev", CVT_B "16do_always_noconvEv",
                CVT_B "13do_max_lengthEv", CVT_B "11do_encodingEv"}));
        v.push_back(facet_vtable(kCodecvtC,
            {C_CVT_C "D1Ev", C_CVT_C "D0Ev", CVT_B "16do_always_noconvEv", CVT_B "13do_max_lengthEv",
                CVT_B "11do_encodingEv", CVT_C "5do_inERS0_PKcS4_RS4_PcS6_RS6_",
                CVT_C "6do_outERS0_PKcS4_RS4_PcS6_RS6_", CVT_C "10do_unshiftERS0_PcS3_RS3_",
                CVT_C "9do_lengthERKS0_PKcS5_j"}));
        v.push_back(facet_vtable(kCodecvtW,
            {C_CVT_W "D1Ev", C_CVT_W "D0Ev", CVT_W "16do_always_noconvEv", CVT_W "13do_max_lengthEv",
                CVT_W "11do_encodingEv", CVT_W "5do_inERS0_PKcS4_RS4_PwS6_RS6_",
                CVT_W "6do_outERS0_PKwS4_RS4_PcS6_RS6_", CVT_W "10do_unshiftERS0_PcS3_RS3_",
                CVT_W "9do_lengthERKS0_PKcS5_j"}));
        v.push_back(facet_vtable(kCollateC,
            {C_COLL_C "D1Ev", C_COLL_C "D0Ev", COLL_C "10do_compareEPKcS2_S2_S2_", COLL_C "12do_transformEPKcS2_",
                COLL_C "7do_hashEPKcS2_"}));
        v.push_back(facet_vtable(kCollateW,
            {C_COLL_W "D1Ev", C_COLL_W "D0Ev", COLL_W "10do_compareEPKwS2_S2_S2_", COLL_W "12do_transformEPKwS2_",
                COLL_W "7do_hashEPKwS2_"}));
        v.push_back(facet_vtable(kMessagesBase, {"_ZNSt13messages_baseD1Ev", "_ZNSt13messages_baseD0Ev"}));
        v.push_back(facet_vtable(kMessagesC,
            {C_MSG_C "D1Ev", C_MSG_C "D0Ev", MSG_C "7do_openERKSsRKSt6locale", MSG_C "6do_getEiiiRKSs",
                MSG_C "8do_closeEi"}));
        v.push_back(facet_vtable(kMessagesW,
            {C_MSG_W "D1Ev", C_MSG_W "D0Ev", MSG_W "7do_openERKSsRKSt6locale",
                MSG_W "6do_getEiiiRKSbIwSt11char_traitsIwESaIwEE", MSG_W "8do_closeEi"}));
        v.push_back(facet_vtable(kNumpunctC,
            {C_NP_C "D1Ev", C_NP_C "D0Ev", NP_C "16do_decimal_pointEv", NP_C "16do_thousands_sepEv",
                NP_C "11do_groupingEv", NP_C "12do_falsenameEv", NP_C "11do_truenameEv"}));
        v.push_back(facet_vtable(kNumpunctW,
            {C_NP_W "D1Ev", C_NP_W "D0Ev", NP_W "16do_decimal_pointEv", NP_W "16do_thousands_sepEv",
                NP_W "11do_groupingEv", NP_W "12do_falsenameEv", NP_W "11do_truenameEv"}));
        v.push_back(facet_vtable(kNumGetC,
            {C_NG_C "D1Ev", C_NG_C "D0Ev", NG_C NG_GET "b", NG_C NG_GET "t", NG_C NG_GET "j", NG_C NG_GET "l",
                NG_C NG_GET "m", NG_C NG_GET "x", NG_C NG_GET "y", NG_C NG_GET "f", NG_C NG_GET "d", NG_C NG_GET "e",
                NG_C NG_GET "Pv"}));
        v.push_back(facet_vtable(kNumGetW,
            {C_NG_W "D1Ev", C_NG_W "D0Ev", NG_W NG_GET "b", NG_W NG_GET "t", NG_W NG_GET "j", NG_W NG_GET "l",
                NG_W NG_GET "m", NG_W NG_GET "x", NG_W NG_GET "y", NG_W NG_GET "f", NG_W NG_GET "d", NG_W NG_GET "e",
                NG_W NG_GET "Pv"}));
        v.push_back(facet_vtable(kNumPutC,
            {C_NPUT_C "D1Ev", C_NPUT_C "D0Ev", NPUT_C NPUT_PUT "cb", NPUT_C NPUT_PUT "cl", NPUT_C NPUT_PUT "cm",
                NPUT_C NPUT_PUT "cx", NPUT_C NPUT_PUT "cy", NPUT_C NPUT_PUT "cd", NPUT_C NPUT_PUT "ce",
                NPUT_C NPUT_PUT "cPKv"}));
        v.push_back(facet_vtable(kNumPutW,
            {C_NPUT_W "D1Ev", C_NPUT_W "D0Ev", NPUT_W NPUT_PUT "wb", NPUT_W NPUT_PUT "wl", NPUT_W NPUT_PUT "wm",
                NPUT_W NPUT_PUT "wx", NPUT_W NPUT_PUT "wy", NPUT_W NPUT_PUT "wd", NPUT_W NPUT_PUT "we",
                NPUT_W NPUT_PUT "wPKv"}));
        v.push_back(facet_vtable(kMoneyBase, {"_ZNSt10money_baseD1Ev", "_ZNSt10money_baseD0Ev"}));
        // _Mpunct and the four moneypuncts: [~D1, ~D0, do_decimal_point, do_thousands_sep, do_grouping,
        // do_curr_symbol, do_positive_sign, do_negative_sign, do_frac_digits, do_pos_format, do_neg_format].
        const auto mpunct = [](std::string_view cls, std::string_view d1, std::string_view d0, bool wide) {
            if (wide)
                return facet_vtable(cls,
                    {d1, d0, MP_W "16do_decimal_pointEv", MP_W "16do_thousands_sepEv", MP_W "11do_groupingEv",
                        MP_W "14do_curr_symbolEv", MP_W "16do_positive_signEv", MP_W "16do_negative_signEv",
                        MP_W "14do_frac_digitsEv", MP_W "13do_pos_formatEv", MP_W "13do_neg_formatEv"});
            return facet_vtable(cls,
                {d1, d0, MP_C "16do_decimal_pointEv", MP_C "16do_thousands_sepEv", MP_C "11do_groupingEv",
                    MP_C "14do_curr_symbolEv", MP_C "16do_positive_signEv", MP_C "16do_negative_signEv",
                    MP_C "14do_frac_digitsEv", MP_C "13do_pos_formatEv", MP_C "13do_neg_formatEv"});
        };
        v.push_back(mpunct(kMpunctC, C_MP_C "D1Ev", C_MP_C "D0Ev", false));
        v.push_back(mpunct(kMpunctW, C_MP_W "D1Ev", C_MP_W "D0Ev", true));
        v.push_back(mpunct(kMoneypunctC0, C_MPC0 "D1Ev", C_MPC0 "D0Ev", false));
        v.push_back(mpunct(kMoneypunctC1, C_MPC1 "D1Ev", C_MPC1 "D0Ev", false));
        v.push_back(mpunct(kMoneypunctW0, C_MPW0 "D1Ev", C_MPW0 "D0Ev", true));
        v.push_back(mpunct(kMoneypunctW1, C_MPW1 "D1Ev", C_MPW1 "D0Ev", true));
        v.push_back(facet_vtable(kMoneyGetC, {C_MG_C "D1Ev", C_MG_C "D0Ev", MG_C MG_GET "e", MG_C MG_GET "Ss"}));
        v.push_back(facet_vtable(kMoneyGetW,
            {C_MG_W "D1Ev", C_MG_W "D0Ev", MG_W MG_GET "e", MG_W MG_GET "SbIwS2_SaIwEE"}));
        v.push_back(facet_vtable(kMoneyPutC,
            {C_MPUT_C "D1Ev", C_MPUT_C "D0Ev", MPUT_C "6do_putES3_bRSt8ios_basece", MPUT_C "6do_putES3_bRSt8ios_basecRKSs"}));
        v.push_back(facet_vtable(kMoneyPutW,
            {C_MPUT_W "D1Ev", C_MPUT_W "D0Ev", MPUT_W "6do_putES3_bRSt8ios_basewe",
                MPUT_W "6do_putES3_bRSt8ios_basewRKSbIwS2_SaIwEE"}));
        v.push_back(facet_vtable(kTimeBase, {"_ZNSt9time_baseD1Ev", "_ZNSt9time_baseD0Ev"}));
        v.push_back(facet_vtable(kTimeGetC,
            {C_TG_C "D1Ev", C_TG_C "D0Ev", TG_C "13do_date_orderEv", TG_C "11do_get_time" TG_ARGS,
                TG_C "11do_get_date" TG_ARGS, TG_C "14do_get_weekday" TG_ARGS, TG_C "16do_get_monthname" TG_ARGS,
                TG_C "11do_get_year" TG_ARGS}));
        v.push_back(facet_vtable(kTimeGetW,
            {C_TG_W "D1Ev", C_TG_W "D0Ev", TG_W "13do_date_orderEv", TG_W "11do_get_time" TG_ARGS,
                TG_W "11do_get_date" TG_ARGS, TG_W "14do_get_weekday" TG_ARGS, TG_W "16do_get_monthname" TG_ARGS,
                TG_W "11do_get_year" TG_ARGS}));
        v.push_back(facet_vtable(kTimePutC,
            {C_TPUT_C "D1Ev", C_TPUT_C "D0Ev", TPUT_C "6do_putES3_RSt8ios_basecPKSt2tmcc"}));
        v.push_back(facet_vtable(kTimePutW,
            {C_TPUT_W "D1Ev", C_TPUT_W "D0Ev", TPUT_W "6do_putES3_RSt8ios_basewPKSt2tmcc"}));
        return v;
    }();
    return defs;
}

const std::vector<TypeDef>& facet_type_defs()
{
    using K = TypeDef::Kind;
    static const std::vector<TypeDef> defs = {
        {kCtypeBase, K::Si, 0, {{kFacetT, 0}}},
        {kCtypeC, K::Si, 0, {{kCtypeBase, 0}}},
        {kCtypeW, K::Si, 0, {{kCtypeBase, 0}}},
        {kCvtBase, K::Si, 0, {{kFacetT, 0}}},
        {kCodecvtC, K::Si, 0, {{kCvtBase, 0}}},
        {kCodecvtW, K::Si, 0, {{kCvtBase, 0}}},
        {kCollateC, K::Si, 0, {{kFacetT, 0}}},
        {kCollateW, K::Si, 0, {{kFacetT, 0}}},
        {kMessagesBase, K::Si, 0, {{kFacetT, 0}}},
        {kMessagesC, K::Si, 0, {{kMessagesBase, 0}}},
        {kMessagesW, K::Si, 0, {{kMessagesBase, 0}}},
        {kNumpunctC, K::Si, 0, {{kFacetT, 0}}},
        {kNumpunctW, K::Si, 0, {{kFacetT, 0}}},
        {kNumGetC, K::Si, 0, {{kFacetT, 0}}},
        {kNumGetW, K::Si, 0, {{kFacetT, 0}}},
        {kNumPutC, K::Si, 0, {{kFacetT, 0}}},
        {kNumPutW, K::Si, 0, {{kFacetT, 0}}},
        {kMoneyBase, K::Si, 0, {{kFacetT, 0}}},
        {kMpunctC, K::Si, 0, {{kMoneyBase, 0}}},
        {kMpunctW, K::Si, 0, {{kMoneyBase, 0}}},
        {kMoneypunctC0, K::Si, 0, {{kMpunctC, 0}}},
        {kMoneypunctC1, K::Si, 0, {{kMpunctC, 0}}},
        {kMoneypunctW0, K::Si, 0, {{kMpunctW, 0}}},
        {kMoneypunctW1, K::Si, 0, {{kMpunctW, 0}}},
        {kMoneyGetC, K::Si, 0, {{kFacetT, 0}}},
        {kMoneyGetW, K::Si, 0, {{kFacetT, 0}}},
        {kMoneyPutC, K::Si, 0, {{kFacetT, 0}}},
        {kMoneyPutW, K::Si, 0, {{kFacetT, 0}}},
        {kTimeBase, K::Si, 0, {{kFacetT, 0}}},
        {kTimeGetC, K::Si, 0, {{kTimeBase, 0}}},
        {kTimeGetW, K::Si, 0, {{kTimeBase, 0}}},
        {kTimePutC, K::Si, 0, {{kFacetT, 0}}},
        {kTimePutW, K::Si, 0, {{kFacetT, 0}}},
    };
    return defs;
}

namespace {

// Each type's _Getcat: (const locale::facet **) -> its category.
template <u32 Cat, u32 (*Make)(PPUContext&)>
u32 getcat_(PPUContext& ctx, u32 ppf)
{
    return getcat(ctx, ppf, Cat, [&] { return Make(ctx); });
}

template <const char* Type>
u32 make_plain_(PPUContext& ctx)
{
    return make_plain(ctx, Type);
}

} // namespace

} // namespace stdcxx

using namespace stdcxx;
using namespace stdcxx::facets;

#define CXX(name, ...)                                                                                                   \
    ::pspocket::hle::Function{{}, name, ::pspocket::recomp::nid_of(name), &::pspocket::hle::detail::thunk<&__VA_ARGS__>, \
        false}
#define CXX_INTERNAL(name, ...)                                                                                          \
    ::pspocket::hle::Function{{}, name, ::pspocket::recomp::nid_of(name), &::pspocket::hle::detail::thunk<&__VA_ARGS__>, \
        true}

void register_stdcxx_facets(Registry& r)
{
    using NK = NumKind;
    using PK = PutKind;
    using TK = TimeKind;
    r.add(kModule,
        {
            // the bases
            CXX("_ZNSt10ctype_baseD0Ev", base_dtor_<kCtypeBase, true>),
            CXX("_ZNSt10ctype_baseD1Ev", base_dtor_<kCtypeBase, false>),
            CXX("_ZNSt10ctype_baseD2Ev", base_dtor_<kCtypeBase, false>),
            CXX("_ZNSt12codecvt_baseD0Ev", base_dtor_<kCvtBase, true>),
            CXX("_ZNSt12codecvt_baseD1Ev", base_dtor_<kCvtBase, false>),
            CXX("_ZNSt12codecvt_baseD2Ev", base_dtor_<kCvtBase, false>),
            CXX("_ZNSt13messages_baseD0Ev", base_dtor_<kMessagesBase, true>),
            CXX("_ZNSt13messages_baseD1Ev", base_dtor_<kMessagesBase, false>),
            CXX("_ZNSt13messages_baseD2Ev", base_dtor_<kMessagesBase, false>),
            CXX("_ZNSt10money_baseD0Ev", base_dtor_<kMoneyBase, true>),
            CXX("_ZNSt10money_baseD1Ev", base_dtor_<kMoneyBase, false>),
            CXX("_ZNSt10money_baseD2Ev", base_dtor_<kMoneyBase, false>),
            CXX("_ZNSt9time_baseD0Ev", base_dtor_<kTimeBase, true>),
            CXX("_ZNSt9time_baseD1Ev", base_dtor_<kTimeBase, false>),
            CXX("_ZNSt9time_baseD2Ev", base_dtor_<kTimeBase, false>),

            // ctype<char>
            CXX(C_CT_C GETCAT, getcat_<kXCtype, make_ctype_c>), CXX(C_CT_C "D0Ev", ctype_dtor_<1, true>),
            CXX(C_CT_C "D1Ev", ctype_dtor_<1, false>), CXX(CT_C "10do_tolowerEc", ctc_tolower_),
            CXX(CT_C "10do_tolowerEPcPKc", ctc_case_range_<false>), CXX(CT_C "10do_toupperEc", ctc_toupper_),
            CXX(CT_C "10do_toupperEPcPKc", ctc_case_range_<true>), CXX(CT_C "8do_widenEc", ctc_widen_),
            CXX(CT_C "8do_widenEPKcS2_Pc", ctc_widen_range_), CXX(CT_C "9do_narrowEcc", ctc_narrow_),
            CXX(CT_C "9do_narrowEPKcS2_cPc", ctc_narrow_range_), CXX("_Getctyptab", getctyptab_),
            // ctype<wchar_t>
            CXX(C_CT_W GETCAT, getcat_<kXCtype, make_ctype_w>), CXX(C_CT_W "D0Ev", ctype_dtor_<4, true>),
            CXX(C_CT_W "D1Ev", ctype_dtor_<4, false>), CXX(CT_W "5do_isEsw", ctw_is_),
            CXX(CT_W "5do_isEPKwS2_Ps", ctw_is_range_), CXX(CT_W "10do_scan_isEsPKwS2_", ctw_scan_<true>),
            CXX(CT_W "11do_scan_notEsPKwS2_", ctw_scan_<false>), CXX(CT_W "10do_tolowerEw", ctw_case_<false>),
            CXX(CT_W "10do_tolowerEPwPKw", ctw_case_range_<false>), CXX(CT_W "10do_toupperEw", ctw_case_<true>),
            CXX(CT_W "10do_toupperEPwPKw", ctw_case_range_<true>), CXX(CT_W "8_DowidenEc", ctw_dowiden_),
            CXX(CT_W "8do_widenEc", ctw_dowiden_), CXX(CT_W "8do_widenEPKcS2_Pw", ctw_widen_range_),
            CXX(CT_W "9_DonarrowEwc", ctw_donarrow_), CXX(CT_W "9do_narrowEwc", ctw_donarrow_),
            CXX(CT_W "9do_narrowEPKwS2_cPc", ctw_narrow_range_),

            // codecvt
            CXX(CVT_B "16do_always_noconvEv", cvt_base_noconv_), CXX(CVT_B "13do_max_lengthEv", cvt_base_max_length_),
            CXX(CVT_B "11do_encodingEv", cvt_base_encoding_),
            CXX_INTERNAL(C_CVT_C GETCAT, getcat_<kXCtype, make_codecvt<1>>),
            CXX(C_CVT_C "D0Ev", codecvt_dtor_<1, true>), CXX(C_CVT_C "D1Ev", codecvt_dtor_<1, false>),
            CXX(CVT_C "5do_inERS0_PKcS4_RS4_PcS6_RS6_", cvtc_inout_),
            CXX(CVT_C "6do_outERS0_PKcS4_RS4_PcS6_RS6_", cvtc_inout_),
            CXX(CVT_C "10do_unshiftERS0_PcS3_RS3_", cvtc_unshift_), CXX(CVT_C "9do_lengthERKS0_PKcS5_j", cvtc_length_),
            CXX(C_CVT_W GETCAT, getcat_<kXCtype, make_codecvt<4>>), CXX(C_CVT_W "D0Ev", codecvt_dtor_<4, true>),
            CXX(C_CVT_W "D1Ev", codecvt_dtor_<4, false>), CXX(CVT_W "16do_always_noconvEv", cvtw_noconv_),
            CXX(CVT_W "13do_max_lengthEv", cvtw_max_length_), CXX(CVT_W "11do_encodingEv", cvtw_encoding_),
            CXX(CVT_W "5do_inERS0_PKcS4_RS4_PwS6_RS6_", cvtw_in_),
            CXX(CVT_W "6do_outERS0_PKwS4_RS4_PcS6_RS6_", cvtw_out_),
            CXX(CVT_W "10do_unshiftERS0_PcS3_RS3_", cvtw_unshift_), CXX(CVT_W "9do_lengthERKS0_PKcS5_j", cvtw_length_),

            // collate
            CXX(C_COLL_C GETCAT, getcat_<kXCollate, make_collate<1>>), CXX(C_COLL_C "D0Ev", collate_dtor_<1, true>),
            CXX(C_COLL_C "D1Ev", collate_dtor_<1, false>), CXX(COLL_C "10do_compareEPKcS2_S2_S2_", collate_compare_<1>),
            CXX(COLL_C "12do_transformEPKcS2_", collate_transform_<1>), CXX(COLL_C "7do_hashEPKcS2_", collate_hash_<1>),
            CXX(C_COLL_W GETCAT, getcat_<kXCollate, make_collate<4>>), CXX(C_COLL_W "D0Ev", collate_dtor_<4, true>),
            CXX(C_COLL_W "D1Ev", collate_dtor_<4, false>), CXX(COLL_W "10do_compareEPKwS2_S2_S2_", collate_compare_<4>),
            CXX(COLL_W "12do_transformEPKwS2_", collate_transform_<4>), CXX(COLL_W "7do_hashEPKwS2_", collate_hash_<4>),

            // messages
            CXX(C_MSG_C GETCAT, getcat_<kXMessages, make_plain_<kMessagesC>>),
            CXX(C_MSG_C "D0Ev", messages_dtor_<1, true>), CXX(C_MSG_C "D1Ev", messages_dtor_<1, false>),
            CXX(MSG_C "7do_openERKSsRKSt6locale", messages_open_), CXX(MSG_C "6do_getEiiiRKSs", messages_get_<1>),
            CXX(MSG_C "8do_closeEi", messages_close_),
            CXX(C_MSG_W GETCAT, getcat_<kXMessages, make_plain_<kMessagesW>>),
            CXX(C_MSG_W "D0Ev", messages_dtor_<4, true>), CXX(C_MSG_W "D1Ev", messages_dtor_<4, false>),
            CXX(MSG_W "7do_openERKSsRKSt6locale", messages_open_),
            CXX(MSG_W "6do_getEiiiRKSbIwSt11char_traitsIwESaIwEE", messages_get_<4>),
            CXX(MSG_W "8do_closeEi", messages_close_),

            // numpunct
            CXX_INTERNAL(C_NP_C GETCAT, getcat_<kXNumeric, make_numpunct<1>>),
            CXX(C_NP_C "D0Ev", numpunct_dtor_<1, true>), CXX(C_NP_C "D1Ev", numpunct_dtor_<1, false>),
            CXX(C_NP_C "5_InitERKSt8_Locinfo", numpunct_init_<1>), CXX(NP_C "16do_decimal_pointEv", numpunct_dp_<1>),
            CXX(NP_C "16do_thousands_sepEv", numpunct_sep_<1>),
            CXX(NP_C "11do_groupingEv", numpunct_str_<1, NpL<1>::kGrouping, 1>),
            CXX(NP_C "12do_falsenameEv", numpunct_str_<1, NpL<1>::kFalse, 1>),
            CXX(NP_C "11do_truenameEv", numpunct_str_<1, NpL<1>::kTrue, 1>),
            CXX(C_NP_W GETCAT, getcat_<kXNumeric, make_numpunct<4>>), CXX(C_NP_W "D0Ev", numpunct_dtor_<4, true>),
            CXX(C_NP_W "D1Ev", numpunct_dtor_<4, false>), CXX(C_NP_W "5_InitERKSt8_Locinfo", numpunct_init_<4>),
            CXX(NP_W "16do_decimal_pointEv", numpunct_dp_<4>), CXX(NP_W "16do_thousands_sepEv", numpunct_sep_<4>),
            CXX(NP_W "11do_groupingEv", numpunct_str_<4, NpL<4>::kGrouping, 1>),
            CXX(NP_W "12do_falsenameEv", numpunct_str_<4, NpL<4>::kFalse, 4>),
            CXX(NP_W "11do_truenameEv", numpunct_str_<4, NpL<4>::kTrue, 4>),

            // num_get<char>
            CXX_INTERNAL(C_NG_C GETCAT, getcat_<kXNumeric, make_plain_<kNumGetC>>),
            CXX(C_NG_C "D0Ev", numget_dtor_<1, true>), CXX(C_NG_C "D1Ev", numget_dtor_<1, false>),
            CXX(NG_C NG_GET "b", numget_c_<NK::Bool>), CXX(NG_C NG_GET "t", numget_c_<NK::UShort>),
            CXX(NG_C NG_GET "j", numget_c_<NK::UInt>), CXX(NG_C NG_GET "l", numget_c_<NK::Long>),
            CXX(NG_C NG_GET "m", numget_c_<NK::ULong>), CXX(NG_C NG_GET "x", numget_c_<NK::LLong>),
            CXX(NG_C NG_GET "y", numget_c_<NK::ULLong>), CXX(NG_C NG_GET "f", numget_c_<NK::Float>),
            CXX(NG_C NG_GET "d", numget_c_<NK::Double>), CXX(NG_C NG_GET "e", numget_c_<NK::LDouble>),
            CXX(NG_C NG_GET "Pv", numget_c_<NK::Ptr>),
            CXX(NG_C "8_GetifldEPcRS3_S6_NSt5_IosbIiE9_FmtflagsERKSt6locale", getifld_<1>),
            CXX(NG_C "8_GetffldEPcRS3_S6_RKSt6locale", getffld_<1>),
            // num_get<wchar_t>
            CXX(C_NG_W GETCAT, getcat_<kXNumeric, make_plain_<kNumGetW>>), CXX(C_NG_W "D0Ev", numget_dtor_<4, true>),
            CXX(C_NG_W "D1Ev", numget_dtor_<4, false>), CXX(NG_W NG_GET "b", numget_w_<NK::Bool>),
            CXX(NG_W NG_GET "t", numget_w_<NK::UShort>), CXX(NG_W NG_GET "j", numget_w_<NK::UInt>),
            CXX(NG_W NG_GET "l", numget_w_<NK::Long>), CXX(NG_W NG_GET "m", numget_w_<NK::ULong>),
            CXX(NG_W NG_GET "x", numget_w_<NK::LLong>), CXX(NG_W NG_GET "y", numget_w_<NK::ULLong>),
            CXX(NG_W NG_GET "f", numget_w_<NK::Float>), CXX(NG_W NG_GET "d", numget_w_<NK::Double>),
            CXX(NG_W NG_GET "e", numget_w_<NK::LDouble>), CXX(NG_W NG_GET "Pv", numget_w_<NK::Ptr>),
            CXX(NG_W "8_GetifldEPcRS3_S6_NSt5_IosbIiE9_FmtflagsERKSt6locale", getifld_<4>),
            CXX(NG_W "8_GetffldEPcRS3_S6_RKSt6locale", getffld_<4>),

            // num_put<char>
            CXX_INTERNAL(C_NPUT_C GETCAT, getcat_<kXNumeric, make_plain_<kNumPutC>>),
            CXX(C_NPUT_C "D0Ev", numput_dtor_<1, true>), CXX(C_NPUT_C "D1Ev", numput_dtor_<1, false>),
            CXX(NPUT_C NPUT_PUT "cb", numput_<1, PK::Bool>), CXX(NPUT_C NPUT_PUT "cl", numput_<1, PK::Long>),
            CXX(NPUT_C NPUT_PUT "cm", numput_<1, PK::ULong>), CXX(NPUT_C NPUT_PUT "cx", numput_<1, PK::LLong>),
            CXX(NPUT_C NPUT_PUT "cy", numput_<1, PK::ULLong>), CXX(NPUT_C NPUT_PUT "cd", numput_f_<1, PK::Double>),
            CXX(NPUT_C NPUT_PUT "ce", numput_f_<1, PK::LDouble>), CXX(NPUT_C NPUT_PUT "cPKv", numput_<1, PK::Ptr>),
            CXX(NPUT_C "5_IputES3_RSt8ios_basecPcj", iput_<1>),
            CXX(NPUT_C "5_FputES3_RSt8ios_basecPKcjjjj", fput_<1>),
            // num_put<wchar_t>
            CXX(C_NPUT_W GETCAT, getcat_<kXNumeric, make_plain_<kNumPutW>>),
            CXX(C_NPUT_W "D0Ev", numput_dtor_<4, true>), CXX(C_NPUT_W "D1Ev", numput_dtor_<4, false>),
            CXX(NPUT_W NPUT_PUT "wb", numput_<4, PK::Bool>), CXX(NPUT_W NPUT_PUT "wl", numput_<4, PK::Long>),
            CXX(NPUT_W NPUT_PUT "wm", numput_<4, PK::ULong>), CXX(NPUT_W NPUT_PUT "wx", numput_<4, PK::LLong>),
            CXX(NPUT_W NPUT_PUT "wy", numput_<4, PK::ULLong>), CXX(NPUT_W NPUT_PUT "wd", numput_f_<4, PK::Double>),
            CXX(NPUT_W NPUT_PUT "we", numput_f_<4, PK::LDouble>), CXX(NPUT_W NPUT_PUT "wPKv", numput_<4, PK::Ptr>),
            CXX(NPUT_W "5_IputES3_RSt8ios_basewPcj", iput_<4>),
            CXX(NPUT_W "5_FputES3_RSt8ios_basewPKcjjjj", fput_<4>),

            // _Mpunct
            CXX(C_MP_C "C2Ejb", mpunct_ctor_<1>), CXX(C_MP_C "C2ERKSt8_Locinfojb", mpunct_ctor_info_<1>),
            CXX(C_MP_C "5_InitERKSt8_Locinfo", mpunct_init_<1>), CXX(C_MP_C "D0Ev", mpunct_dtor_<1, true>),
            CXX(C_MP_C "D1Ev", mpunct_dtor_<1, false>), CXX(C_MP_C "D2Ev", mpunct_dtor_<1, false>),
            CXX(MP_C "16do_decimal_pointEv", mpunct_dp_<1>), CXX(MP_C "16do_thousands_sepEv", mpunct_sep_<1>),
            CXX(MP_C "11do_groupingEv", mpunct_str_<1, MpL<1>::kGrouping, 1>),
            CXX(MP_C "14do_curr_symbolEv", mpunct_str_<1, MpL<1>::kCurr, 1>),
            CXX(MP_C "16do_positive_signEv", mpunct_str_<1, MpL<1>::kPlus, 1>),
            CXX(MP_C "16do_negative_signEv", mpunct_str_<1, MpL<1>::kMinus, 1>),
            CXX(MP_C "14do_frac_digitsEv", mpunct_frac_<1>),
            CXX(MP_C "13do_pos_formatEv", mpunct_fmt_<1, MpL<1>::kPlusfmt>),
            CXX(MP_C "13do_neg_formatEv", mpunct_fmt_<1, MpL<1>::kMinusfmt>),
            CXX(C_MP_W "C2Ejb", mpunct_ctor_<4>), CXX(C_MP_W "C2ERKSt8_Locinfojb", mpunct_ctor_info_<4>),
            CXX(C_MP_W "5_InitERKSt8_Locinfo", mpunct_init_<4>), CXX(C_MP_W "D0Ev", mpunct_dtor_<4, true>),
            CXX(C_MP_W "D1Ev", mpunct_dtor_<4, false>), CXX(C_MP_W "D2Ev", mpunct_dtor_<4, false>),
            CXX(MP_W "16do_decimal_pointEv", mpunct_dp_<4>), CXX(MP_W "16do_thousands_sepEv", mpunct_sep_<4>),
            CXX(MP_W "11do_groupingEv", mpunct_str_<4, MpL<4>::kGrouping, 1>),
            CXX(MP_W "14do_curr_symbolEv", mpunct_str_<4, MpL<4>::kCurr, 4>),
            CXX(MP_W "16do_positive_signEv", mpunct_str_<4, MpL<4>::kPlus, 4>),
            CXX(MP_W "16do_negative_signEv", mpunct_str_<4, MpL<4>::kMinus, 4>),
            CXX(MP_W "14do_frac_digitsEv", mpunct_frac_<4>),
            CXX(MP_W "13do_pos_formatEv", mpunct_fmt_<4, MpL<4>::kPlusfmt>),
            CXX(MP_W "13do_neg_formatEv", mpunct_fmt_<4, MpL<4>::kMinusfmt>),
            // moneypunct
            CXX(C_MPC0 GETCAT, getcat_<kXMonetary, make_moneypunct<1, false>>),
            CXX(C_MPC0 "D0Ev", moneypunct_dtor_<1, false, true>), CXX(C_MPC0 "D1Ev", moneypunct_dtor_<1, false, false>),
            CXX(C_MPC1 GETCAT, getcat_<kXMonetary, make_moneypunct<1, true>>),
            CXX(C_MPC1 "D0Ev", moneypunct_dtor_<1, true, true>), CXX(C_MPC1 "D1Ev", moneypunct_dtor_<1, true, false>),
            CXX(C_MPW0 GETCAT, getcat_<kXMonetary, make_moneypunct<4, false>>),
            CXX(C_MPW0 "D0Ev", moneypunct_dtor_<4, false, true>), CXX(C_MPW0 "D1Ev", moneypunct_dtor_<4, false, false>),
            CXX(C_MPW1 GETCAT, getcat_<kXMonetary, make_moneypunct<4, true>>),
            CXX(C_MPW1 "D0Ev", moneypunct_dtor_<4, true, true>), CXX(C_MPW1 "D1Ev", moneypunct_dtor_<4, true, false>),

            // money_get
            CXX(C_MG_C GETCAT, getcat_<kXMonetary, make_plain_<kMoneyGetC>>),
            CXX(C_MG_C "D0Ev", moneyget_dtor_<1, true>), CXX(C_MG_C "D1Ev", moneyget_dtor_<1, false>),
            CXX(MG_C MG_GET "e", moneyget_c_<false>), CXX(MG_C MG_GET "Ss", moneyget_c_<true>),
            CXX(MG_C "8_GetmfldERS3_S5_bRSt8ios_base", getmfld_<1>),
            CXX(C_MG_W GETCAT, getcat_<kXMonetary, make_plain_<kMoneyGetW>>),
            CXX(C_MG_W "D0Ev", moneyget_dtor_<4, true>), CXX(C_MG_W "D1Ev", moneyget_dtor_<4, false>),
            CXX(MG_W MG_GET "e", moneyget_w_<false>), CXX(MG_W MG_GET "SbIwS2_SaIwEE", moneyget_w_<true>),
            CXX(MG_W "8_GetmfldERS3_S5_bRSt8ios_base", getmfld_<4>),
            // money_put
            CXX(C_MPUT_C GETCAT, getcat_<kXMonetary, make_plain_<kMoneyPutC>>),
            CXX(C_MPUT_C "D0Ev", moneyput_dtor_<1, true>), CXX(C_MPUT_C "D1Ev", moneyput_dtor_<1, false>),
            CXX(MPUT_C "6do_putES3_bRSt8ios_basece", moneyput_ld_<1>),
            CXX(MPUT_C "6do_putES3_bRSt8ios_basecRKSs", moneyput_str_<1>),
            CXX(MPUT_C "8_PutmfldES3_bRSt8ios_basecbSs", putmfld_<1>),
            CXX(C_MPUT_W GETCAT, getcat_<kXMonetary, make_plain_<kMoneyPutW>>),
            CXX(C_MPUT_W "D0Ev", moneyput_dtor_<4, true>), CXX(C_MPUT_W "D1Ev", moneyput_dtor_<4, false>),
            CXX(MPUT_W "6do_putES3_bRSt8ios_basewe", moneyput_ld_<4>),
            CXX(MPUT_W "6do_putES3_bRSt8ios_basewRKSbIwS2_SaIwEE", moneyput_str_<4>),
            CXX(MPUT_W "8_PutmfldES3_bRSt8ios_basewbSbIwS2_SaIwEE", putmfld_<4>),

            // time_get
            CXX(C_TG_C GETCAT, getcat_<kXTime, make_timeget<1>>), CXX(C_TG_C "D0Ev", timeget_dtor_<1, true>),
            CXX(C_TG_C "D1Ev", timeget_dtor_<1, false>), CXX(C_TG_C "5_InitERKSt8_Locinfo", timeget_init_<1>),
            CXX(TG_C "13do_date_orderEv", timeget_order_), CXX(TG_C "11do_get_time" TG_ARGS, timeget_c_<TK::Time>),
            CXX(TG_C "11do_get_date" TG_ARGS, timeget_c_<TK::Date>),
            CXX(TG_C "14do_get_weekday" TG_ARGS, timeget_c_<TK::Weekday>),
            CXX(TG_C "16do_get_monthname" TG_ARGS, timeget_c_<TK::Monthname>),
            CXX(TG_C "11do_get_year" TG_ARGS, timeget_c_<TK::Year>), CXX(TG_C "7_GetintERS3_S5_iiRi", getint_<1>),
            CXX(C_TG_W GETCAT, getcat_<kXTime, make_timeget<4>>), CXX(C_TG_W "D0Ev", timeget_dtor_<4, true>),
            CXX(C_TG_W "D1Ev", timeget_dtor_<4, false>), CXX(C_TG_W "5_InitERKSt8_Locinfo", timeget_init_<4>),
            CXX(TG_W "13do_date_orderEv", timeget_order_), CXX(TG_W "11do_get_time" TG_ARGS, timeget_w_<TK::Time>),
            CXX(TG_W "11do_get_date" TG_ARGS, timeget_w_<TK::Date>),
            CXX(TG_W "14do_get_weekday" TG_ARGS, timeget_w_<TK::Weekday>),
            CXX(TG_W "16do_get_monthname" TG_ARGS, timeget_w_<TK::Monthname>),
            CXX(TG_W "11do_get_year" TG_ARGS, timeget_w_<TK::Year>), CXX(TG_W "7_GetintERS3_S5_iiRi", getint_<4>),
            CXX("_ZSt10_GetloctxtIcSt19istreambuf_iteratorIcSt11char_traitsIcEEEiRT0_S5_jPKT_", getloctxt_<1>),
            CXX("_ZSt10_GetloctxtIwSt19istreambuf_iteratorIwSt11char_traitsIwEEEiRT0_S5_jPKT_", getloctxt_<4>),
            // time_put
            CXX(C_TPUT_C GETCAT, getcat_<kXTime, make_plain_<kTimePutC>>), CXX(C_TPUT_C "D0Ev", timeput_dtor_<1, true>),
            CXX(C_TPUT_C "D1Ev", timeput_dtor_<1, false>), CXX(TPUT_C "6do_putES3_RSt8ios_basecPKSt2tmcc", timeput_<1>),
            CXX(C_TPUT_W GETCAT, getcat_<kXTime, make_plain_<kTimePutW>>), CXX(C_TPUT_W "D0Ev", timeput_dtor_<4, true>),
            CXX(C_TPUT_W "D1Ev", timeput_dtor_<4, false>), CXX(TPUT_W "6do_putES3_RSt8ios_basewPKSt2tmcc", timeput_<4>),

            // _Maklocstr, _Maklocchr, _Maklocbyte for wchar_t
            CXX("_ZSt10_MaklocstrIwEPT_PKcS1_RKSt7_Cvtvec", maklocstr_w_),
            CXX("_ZSt10_MaklocchrIwET_cPS0_RKSt7_Cvtvec", maklocchr_w_),
            CXX("_ZSt11_MaklocbyteIwEcT_RKSt7_Cvtvec", maklocbyte_w_),
        });
}

} // namespace pspocket::hle

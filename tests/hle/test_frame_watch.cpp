#include <catch_amalgamated.hpp>

#include "pspocket/loader/frame_watch.hpp"

#include <vector>

using namespace pspocket;
using loader::FrameWatch;

namespace {

FrameWatch::Thumb filled(u8 v)
{
    FrameWatch::Thumb t;
    t.fill(v);
    return t;
}

// A scene: a dim gradient (never exactly 0), as fogged rendering gives.
FrameWatch::Thumb scene(u8 base)
{
    FrameWatch::Thumb t;
    for (usize i = 0; i < t.size(); i++) t[i] = static_cast<u8>(base + (i / 3) % 40);
    return t;
}

constexpr u64 kFrame = 33'333'333;

} // namespace

TEST_CASE("frame watch: a surface nothing drew, flicker, hitches, a frozen picture (D-1064)", "[loader]")
{
    std::vector<FrameWatch::Event> got;
    FrameWatch w;
    w.on_event = [&](const FrameWatch::Event& e) { got.push_back(e); };
    u64 t = 0, flip = 0;
    auto feed = [&](const FrameWatch::Thumb& th) { w.frame(th, ++flip, t += kFrame); };

    SECTION("a night scene is not black; the lower half exactly black for two frames is")
    {
        for (int i = 0; i < 40; i++) feed(scene(2));
        CHECK(got.empty());
        FrameWatch::Thumb half = scene(30);
        std::fill(half.begin() + half.size() / 2, half.end(), 0);
        feed(half);
        CHECK(got.empty()); // one frame: a cut can do that
        feed(half);
        REQUIRE(got.size() == 1);
        CHECK(got[0].kind == FrameWatch::Kind::PartlyBlack);
        CHECK(got[0].value == Catch::Approx(0.5));
        feed(filled(0)); // a fade to black is not a glitch
        feed(filled(0));
        CHECK(got.size() == 1);
    }
    SECTION("A B A is flicker; A B B is a cut")
    {
        for (int i = 0; i < 5; i++) feed(scene(20));
        feed(scene(120)); // a cut
        feed(scene(120));
        CHECK(got.empty());
        feed(scene(20));
        feed(scene(120));
        REQUIRE(got.size() == 1);
        CHECK(got[0].kind == FrameWatch::Kind::Flicker);
        feed(scene(20));
        CHECK(got.size() == 1); // rate-limited: counted, not reported again
        CHECK(w.counts[static_cast<usize>(FrameWatch::Kind::Flicker)] >= 2);
    }
    SECTION("over half a second between frames while flipping is a hitch; a frozen picture after 10 s")
    {
        for (int i = 0; i < 60; i++) feed(scene(i % 2 ? 20 : 22));
        t += 800'000'000;
        feed(scene(20));
        REQUIRE(got.size() == 1);
        CHECK(got[0].kind == FrameWatch::Kind::Hitch);
        CHECK(got[0].value > 800);
        for (int i = 0; i < 31 * 11; i++) feed(scene(50));
        REQUIRE(got.size() == 2);
        CHECK(got[1].kind == FrameWatch::Kind::Frozen);
    }
    SECTION("the thumbnail of BGRA pixels, and the recent ones oldest first")
    {
        std::vector<u8> px(128 * 72 * 4, 0);
        for (usize i = 0; i < px.size(); i += 4) px[i + 2] = 200; // red
        const auto th = FrameWatch::thumbnail(px.data(), 128, 72, 128 * 4);
        CHECK(th[0] == 0);
        CHECK(th[2] == 200);
        w.frame(scene(1), 1, kFrame);
        w.frame(scene(2), 2, 2 * kFrame);
        const auto r = w.recent();
        REQUIRE(r.size() == 2);
        CHECK(r[0][0] == 1);
        CHECK(r[1][0] == 2);
    }
}

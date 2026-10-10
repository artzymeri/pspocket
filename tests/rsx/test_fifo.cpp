// FIFO parser tests: method headers, increment/non-increment, NOPs, old/new jumps, calls and returns (with nesting),
// jump-to-self, split packets, errors and recovery, semaphores, labels, the reference register, and the RSX thread.
// Command buffers are synthetic, built with CommandBuilder.

#include <catch_amalgamated.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "rsx_test_util.hpp"

using namespace pspocket;
using namespace pspocket::rsx;
using namespace pspocket::rsx::test;
using Kind = RecordingBackend::Kind;

TEST_CASE("methods write registers and get follows put")
{
    Fixture f;
    auto b = f.at(0);
    b.method(m::NV4097_SET_BLEND_ENABLE, 1);
    b.method(m::NV4097_SET_DEPTH_FUNC, {0x203, 1, 0}); // DEPTH_FUNC, DEPTH_MASK, DEPTH_TEST_ENABLE
    REQUIRE(f.submit(b) == Rsx::RunStatus::Idle);

    CHECK(f.state().reg(m::NV4097_SET_BLEND_ENABLE) == 1);
    CHECK(f.state().reg(m::NV4097_SET_DEPTH_FUNC) == 0x203);
    CHECK(f.state().reg(m::NV4097_SET_DEPTH_MASK) == 1);
    CHECK(f.state().reg(m::NV4097_SET_DEPTH_TEST_ENABLE) == 0);
    CHECK(f.rsx->get() == b.offset());
    CHECK(f.get_register() == b.offset()); // the guest-visible get register
    CHECK(f.failures.empty());
}

TEST_CASE("non-incrementing methods send every argument to the same register")
{
    Fixture f;
    auto b = f.at(0);
    b.method_ni(m::NV4097_SET_ALPHA_REF, {1, 2, 3});
    REQUIRE(f.submit(b) == Rsx::RunStatus::Idle);
    CHECK(f.state().reg(m::NV4097_SET_ALPHA_REF) == 3);
    CHECK(f.state().reg(m::NV4097_SET_ALPHA_REF + 1) == 0); // the next register (BLEND_ENABLE) is untouched

    // Non-incrementing clears: three CLEAR_SURFACE calls
    auto c = f.at(b.offset());
    c.method_ni(m::NV4097_CLEAR_SURFACE, {0x10, 0x20, 0x40});
    REQUIRE(f.submit(c) == Rsx::RunStatus::Idle);
    CHECK(f.backend.clears() == std::vector<u32>{0x10, 0x20, 0x40});
}

TEST_CASE("NOP headers are skipped")
{
    Fixture f;
    auto b = f.at(0);
    b.nop().nop();
    b.method(m::NV4097_CLEAR_SURFACE, 0xf3);
    b.nop();
    REQUIRE(f.submit(b) == Rsx::RunStatus::Idle);
    CHECK(f.backend.clears() == std::vector<u32>{0xf3});
    CHECK(f.rsx->get() == b.offset());
}

TEST_CASE("old and new jumps redirect the FIFO")
{
    Fixture f;
    // 0x000: CLEAR(1), old JUMP 0x100
    // 0x100: CLEAR(2), new JUMP 0x200
    // 0x200: CLEAR(3)
    auto a = f.at(0);
    a.method(m::NV4097_CLEAR_SURFACE, 1).jump(0x100);
    auto b = f.at(0x100);
    b.method(m::NV4097_CLEAR_SURFACE, 2).jump_new(0x200);
    auto c = f.at(0x200);
    c.method(m::NV4097_CLEAR_SURFACE, 3);
    // A command that must not run: right after the first jump.
    f.at(a.offset()).method(m::NV4097_CLEAR_SURFACE, 0xdead);
    REQUIRE(f.submit(c) == Rsx::RunStatus::Idle);
    CHECK(f.backend.clears() == std::vector<u32>{1, 2, 3});
    CHECK(f.rsx->get() == c.offset());
}

TEST_CASE("call and return")
{
    Fixture f;
    // main: CLEAR(1) CALL sub CLEAR(3);  sub at 0x400: CLEAR(2) RETURN
    auto sub = f.at(0x400);
    sub.method(m::NV4097_CLEAR_SURFACE, 2).ret();
    auto main = f.at(0);
    main.method(m::NV4097_CLEAR_SURFACE, 1).call(0x400).method(m::NV4097_CLEAR_SURFACE, 3);
    REQUIRE(f.submit(main) == Rsx::RunStatus::Idle);
    CHECK(f.backend.clears() == std::vector<u32>{1, 2, 3});
    CHECK(f.rsx->call_depth() == 0);
    CHECK(f.failures.empty());
}

TEST_CASE("the same subroutine can be called repeatedly")
{
    Fixture f;
    auto sub = f.at(0x800);
    sub.method(m::NV4097_CLEAR_SURFACE, 7).ret();
    auto main = f.at(0);
    main.call(0x800).call(0x800).call(0x800);
    REQUIRE(f.submit(main) == Rsx::RunStatus::Idle);
    CHECK(f.backend.clears() == std::vector<u32>{7, 7, 7});
}

TEST_CASE("nested calls use the return stack when it is deep enough")
{
    Config cfg = test_config();
    cfg.call_stack_depth = 4;
    Fixture f(cfg);
    // main: CALL s1, CLEAR(4)   s1: CLEAR(1) CALL s2 CLEAR(3) RET   s2: CALL s3 RET   s3: CLEAR(2) RET
    f.at(0x300).method(m::NV4097_CLEAR_SURFACE, 2).ret();
    f.at(0x200).call(0x300).ret();
    f.at(0x100).method(m::NV4097_CLEAR_SURFACE, 1).call(0x200).method(m::NV4097_CLEAR_SURFACE, 3).ret();
    auto main = f.at(0);
    main.call(0x100).method(m::NV4097_CLEAR_SURFACE, 4);
    REQUIRE(f.submit(main) == Rsx::RunStatus::Idle);
    CHECK(f.backend.clears() == std::vector<u32>{1, 2, 3, 4});
    CHECK(f.failures.empty());
}

TEST_CASE("a nested call beyond the hardware depth is a FIFO error and the FIFO recovers")
{
    Fixture f; // depth 1, like the NV40 PFIFO
    f.at(0x200).method(m::NV4097_CLEAR_SURFACE, 2).ret();
    f.at(0x100).call(0x200).ret();
    auto main = f.at(0);
    main.call(0x100).method(m::NV4097_CLEAR_SURFACE, 9);
    REQUIRE(f.submit(main) == Rsx::RunStatus::Error);
    REQUIRE(f.failures.size() == 1);
    CHECK(f.failures[0].kind == FailKind::FifoError);
    CHECK(f.failures[0].message.find("nested") != std::string::npos);
    // Recovery skips to put: nothing else ran, and the next submission works.
    CHECK(f.backend.clears().empty());
    CHECK(f.rsx->get() == main.offset());
    auto next = f.at(main.offset());
    next.method(m::NV4097_CLEAR_SURFACE, 5);
    REQUIRE(f.submit(next) == Rsx::RunStatus::Idle);
    CHECK(f.backend.clears() == std::vector<u32>{5});
}

TEST_CASE("RETURN without CALL and malformed words are FIFO errors")
{
    Fixture f;
    auto b = f.at(0);
    b.ret();
    REQUIRE(f.submit(b) == Rsx::RunStatus::Error);
    REQUIRE(f.failures.size() == 1);
    CHECK(f.failures[0].message.find("RETURN") != std::string::npos);

    auto c = f.at(b.offset());
    c.word(0x80000000); // bit 31: not a method, not a jump
    REQUIRE(f.submit(c) == Rsx::RunStatus::Error);
    REQUIRE(f.failures.size() == 2);
    CHECK(f.failures[1].kind == FailKind::FifoError);
    CHECK(f.failures[1].message.find("malformed") != std::string::npos);
}

TEST_CASE("a desync right after a jump is read again from the jump target (D-118)")
{
    // The reader jumps into a stretch it reads before the guest has finished writing it: an unknown method, then a
    // malformed word. A millisecond later it reads the stretch again, written by then.
    Fixture f;
    auto main = f.at(0);
    main.method(m::NV4097_CLEAR_SURFACE, 1).jump(0x1000);
    auto stale = f.at(0x1000);
    stale.word(0x00040080).word(0x12345678).word(0x80000000); // method 0x0080 does not exist; then not a command
    CommandBuilder fresh = f.at(0x1000);
    bool landed = false;
    f.rsx->on_failure = [&](const Failure& x) {
        f.failures.push_back(x);
        if (landed) return;
        landed = true; // the guest's words land
        fresh.method(m::NV4097_CLEAR_SURFACE, 2).nop();
    };
    f.set_put(stale.offset());
    REQUIRE(f.rsx->run_until_idle() == Rsx::RunStatus::Idle);
    CHECK(f.backend.clears() == std::vector<u32>{1, 2}); // what came before the jump ran once
    REQUIRE(f.failures.size() == 1);
    CHECK(f.failures[0].kind == FailKind::UnhandledMethod);
    CHECK(f.rsx->get() == stale.offset());
}

TEST_CASE("a desync that stays is read again twice, then skipped to put (D-118)")
{
    Fixture f;
    auto main = f.at(0);
    main.method(m::NV4097_CLEAR_SURFACE, 1).jump(0x1000);
    auto bad = f.at(0x1000);
    bad.word(0x00040080).word(0x12345678).word(0x80000000);
    f.set_put(bad.offset());
    REQUIRE(f.rsx->run_until_idle() == Rsx::RunStatus::Error);
    CHECK(f.backend.clears() == std::vector<u32>{1});
    REQUIRE(f.failures.size() == 4); // the unknown method on each of the three reads, then the malformed word
    CHECK(f.failures[2].kind == FailKind::UnhandledMethod);
    CHECK(f.failures[3].kind == FailKind::FifoError);
    CHECK(f.failures[3].message.find("malformed") != std::string::npos);
    CHECK(f.rsx->get() == bad.offset());
}

TEST_CASE("a re-read passes the semaphore acquires the reader passed before the desync (D-118)")
{
    // The semaphore moves on while the reader goes the wrong way: waiting for the old value again would never end.
    Fixture f;
    const u32 label = f.layout().label_addr(64);
    f.write32(label, 5);
    auto main = f.at(0);
    main.method(m::NV406E_SET_CONTEXT_DMA_SEMAPHORE, loc::kDmaSemaphoreRW)
        .method(m::NV406E_SEMAPHORE_OFFSET, 64 * 0x10)
        .jump(0x1000);
    auto stale = f.at(0x1000);
    stale.method(m::NV406E_SEMAPHORE_ACQUIRE, 5);
    CommandBuilder fresh = f.at(stale.offset());
    stale.word(0x00040080).word(0x12345678);
    bool landed = false;
    f.rsx->on_failure = [&](const Failure& x) {
        f.failures.push_back(x);
        if (landed) return;
        landed = true;
        f.write32(label, 6);
        fresh.method(m::NV4097_CLEAR_SURFACE, 7);
    };
    f.set_put(stale.offset());
    REQUIRE(f.rsx->run_until_idle() == Rsx::RunStatus::Idle);
    CHECK(f.backend.clears() == std::vector<u32>{7});
    CHECK(f.failures.size() == 1);

    // Past the point of the desync, acquires wait again.
    auto next = f.at(stale.offset());
    next.method(m::NV406E_SEMAPHORE_ACQUIRE, 5);
    REQUIRE(f.submit(next) == Rsx::RunStatus::Blocked);
}

TEST_CASE("a FIFO reset in the middle of a batch takes effect before the next word (D-123)")
{
    // The game sets get and put (sys_rsx_context_attribute 0x001) while the RSX is in a batch: the words after the
    // reset point, past the old put's stretch, were read against the new put until the batch ended.
    Fixture f;
    auto old_stream = f.at(0);
    old_stream.method(m::NV4097_CLEAR_SURFACE, 1).method(m::NV4097_CLEAR_SURFACE, 2).method(m::NV4097_CLEAR_SURFACE, 3);
    auto fresh = f.at(0x2000);
    fresh.method(m::NV4097_CLEAR_SURFACE, 9);
    f.backend.on_clear = [&](u32 mask) {
        if (mask == 1) f.rsx->set_fifo_pointers(0x2000, fresh.offset()); // the game moves the FIFO at the first clear
    };
    f.set_put(old_stream.offset());
    for (int i = 0; i < 4 && f.rsx->run_until_idle(64) != Rsx::RunStatus::Idle; i++) {}
    CHECK(f.backend.clears() == std::vector<u32>{1, 9});
    CHECK(f.rsx->get() == fresh.offset());
    CHECK(f.failures.empty());
}

TEST_CASE("an unmapped FIFO address is a FIFO error")
{
    Fixture f;
    auto b = f.at(0);
    b.jump(0x01000000); // IO offset 16 MB: not mapped
    f.set_put(0x01000010);
    REQUIRE(f.rsx->run_until_idle() == Rsx::RunStatus::Error);
    REQUIRE(f.failures.size() == 1);
    CHECK(f.failures[0].message.find("not mapped") != std::string::npos);
}

TEST_CASE("unhandled methods are reported with name and number")
{
    Fixture f;
    auto b = f.at(0);
    b.method(0x1234 >> 2, 0x55);                   // inside NV4097 space, no such method
    b.method(m::NV4097_SET_COLOR_MASK, 0x01010101); // known, keeps going
    REQUIRE(f.submit(b) == Rsx::RunStatus::Idle);
    REQUIRE(f.failures.size() == 1);
    CHECK(f.failures[0].kind == FailKind::UnhandledMethod);
    CHECK(f.failures[0].method == (0x1234u >> 2));
    CHECK(f.failures[0].message.find("0x1234") != std::string::npos);
    CHECK(f.failures[0].message.find("0x00000055") != std::string::npos);
    CHECK(f.state().reg(m::NV4097_SET_COLOR_MASK) == 0x01010101);
}

TEST_CASE("a packet split by put resumes when put moves")
{
    Fixture f;
    auto b = f.at(0);
    b.method(m::NV4097_CLEAR_SURFACE + 0, {0x10}); // whole packet
    const u32 split_start = b.offset();
    b.method_ni(m::NV4097_CLEAR_SURFACE, {0x11, 0x12, 0x13, 0x14});
    // put stops after the header and two arguments
    f.set_put(split_start + 4 + 8);
    REQUIRE(f.rsx->run_until_idle() == Rsx::RunStatus::Idle);
    CHECK(f.backend.clears() == std::vector<u32>{0x10, 0x11, 0x12});
    CHECK(f.rsx->get() == split_start + 12);
    f.set_put(b.offset());
    REQUIRE(f.rsx->run_until_idle() == Rsx::RunStatus::Idle);
    CHECK(f.backend.clears() == std::vector<u32>{0x10, 0x11, 0x12, 0x13, 0x14});
}

TEST_CASE("jump to self parks the FIFO until the word is patched")
{
    Fixture f;
    auto b = f.at(0);
    b.method(m::NV4097_CLEAR_SURFACE, 1);
    const u32 park = b.offset();
    b.jump(park); // jump to self
    b.method(m::NV4097_CLEAR_SURFACE, 2);
    f.set_put(b.offset());
    REQUIRE(f.rsx->run_until_idle() == Rsx::RunStatus::Blocked);
    CHECK(f.rsx->get() == park);
    CHECK(f.backend.clears() == std::vector<u32>{1});
    REQUIRE(f.rsx->run_until_idle() == Rsx::RunStatus::Blocked); // still parked

    // The guest replaces the jump with a NOP.
    f.write32(Fixture::kCmdEa + park, 0);
    REQUIRE(f.rsx->run_until_idle() == Rsx::RunStatus::Idle);
    CHECK(f.backend.clears() == std::vector<u32>{1, 2});
}

TEST_CASE("set_fifo_pointers resets the parser and writes the control registers")
{
    Fixture f;
    auto b = f.at(0x1000);
    b.method(m::NV4097_CLEAR_SURFACE, 3);
    f.rsx->set_fifo_pointers(0x1000, b.offset());
    CHECK(f.read32(f.layout().put_addr()) == b.offset());
    CHECK(f.read32(f.layout().get_addr()) == 0x1000);
    REQUIRE(f.rsx->run_until_idle() == Rsx::RunStatus::Idle);
    CHECK(f.backend.clears() == std::vector<u32>{3});
    CHECK(f.rsx->fifo_location() == FifoLocation::Io);
}

TEST_CASE("a FIFO whose first get is not IO mapped lives in local memory")
{
    // GTA IV's libgcm puts its push buffer in local memory (RPCS3 RSXFIFO.cpp g_fifo_location).
    GuestMemory mem;
    REQUIRE(mem.reserve());
    REQUIRE(mem.commit(PS_MEM_RSX_LOCAL_BASE, 0x100000));
    Config cfg = test_config();
    REQUIRE(initialize_context_memory(mem, cfg.layout));
    RecordingBackend backend;
    Rsx rsx(mem, backend, cfg);
    CommandBuilder b(mem.base(), PS_MEM_RSX_LOCAL_BASE + 0x8000, 0x8000);
    b.method(m::NV4097_CLEAR_SURFACE, 0x80);
    rsx.set_fifo_pointers(0x8000, b.offset());
    CHECK(rsx.fifo_location() == FifoLocation::Local);
    REQUIRE(rsx.run_until_idle() == Rsx::RunStatus::Idle);
    CHECK(backend.clears() == std::vector<u32>{0x80});
}

TEST_CASE("NV406E semaphore acquire blocks until the label matches")
{
    Fixture f;
    const u32 label = f.layout().label_addr(64);
    f.write32(label, 0);
    auto b = f.at(0);
    b.method(m::NV406E_SET_CONTEXT_DMA_SEMAPHORE, loc::kDmaSemaphoreRW);
    b.method(m::NV406E_SEMAPHORE_OFFSET, 64 * 0x10);
    const u32 acquire_at = b.offset();
    b.method(m::NV406E_SEMAPHORE_ACQUIRE, 0x1234);
    b.method(m::NV4097_CLEAR_SURFACE, 0x10);
    REQUIRE(f.submit(b) == Rsx::RunStatus::Blocked);
    CHECK(f.rsx->get() == acquire_at + 4); // parked on the acquire's argument
    CHECK(f.backend.clears().empty());

    f.write32(label, 0x1233);
    REQUIRE(f.rsx->run_until_idle() == Rsx::RunStatus::Blocked);
    f.write32(label, 0x1234);
    REQUIRE(f.rsx->run_until_idle() == Rsx::RunStatus::Idle);
    CHECK(f.backend.clears() == std::vector<u32>{0x10});
}

// Kept from D-229 (reading a packet's arguments in one run, tried and not kept): what any reader must do.
TEST_CASE("an acquire blocking inside a packet parks on its own word; the rest of the packet runs after (D-229)")
{
    Fixture f;
    const u32 label = f.layout().label_addr(64);
    f.write32(label, 0);
    auto b = f.at(0);
    b.method(m::NV406E_SET_CONTEXT_DMA_SEMAPHORE, loc::kDmaSemaphoreRW);
    // One packet: SEMAPHORE_OFFSET, then SEMAPHORE_ACQUIRE (the next register).
    const u32 packet_at = b.offset();
    b.method(m::NV406E_SEMAPHORE_OFFSET, {64 * 0x10, 0x1234});
    // A long run of arguments after it: 32 transform constants, then a clear.
    std::vector<u32> constants(32);
    for (u32 i = 0; i < 32; i++) constants[i] = 0x3F800000 + i;
    b.method(m::NV4097_SET_TRANSFORM_CONSTANT_LOAD, 0);
    b.method(m::NV4097_SET_TRANSFORM_CONSTANT, constants);
    b.method(m::NV4097_CLEAR_SURFACE, 0x10);
    REQUIRE(f.submit(b) == Rsx::RunStatus::Blocked);
    CHECK(f.rsx->get() == packet_at + 8); // on the acquire's word, the offset before it taken
    CHECK(f.read32(f.layout().get_addr()) == packet_at + 8);
    CHECK(f.backend.clears().empty());
    f.write32(label, 0x1234);
    REQUIRE(f.rsx->run_until_idle() == Rsx::RunStatus::Idle);
    CHECK(f.backend.clears() == std::vector<u32>{0x10});
    CHECK(f.rsx->get() == b.offset());
    CHECK(f.read32(f.layout().get_addr()) == b.offset()); // the guest's get register published at the end
}

TEST_CASE("semaphore releases and the reference register are written in command order")
{
    Fixture f;
    auto b = f.at(0);
    // NV406E release at label 65
    b.method(m::NV406E_SET_CONTEXT_DMA_SEMAPHORE, loc::kDmaSemaphoreRW);
    b.method(m::NV406E_SEMAPHORE_OFFSET, 65 * 0x10);
    b.method(m::NV406E_SEMAPHORE_RELEASE, 0xCAFE0001);
    // NV4097 texture-read release at label 66, back-end release at label 67 (bytes 0 and 2 exchanged)
    b.method(m::NV4097_SET_CONTEXT_DMA_SEMAPHORE, loc::kDmaSemaphoreRW);
    b.method(m::NV4097_SET_SEMAPHORE_OFFSET, 66 * 0x10);
    b.method(m::NV4097_TEXTURE_READ_SEMAPHORE_RELEASE, 0x11223344);
    b.method(m::NV4097_SET_SEMAPHORE_OFFSET, 67 * 0x10);
    b.method(m::NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE, 0x11223344);
    b.method(m::NV406E_SET_REFERENCE, 0x77);
    REQUIRE(f.submit(b) == Rsx::RunStatus::Idle);
    CHECK(f.read32(f.layout().label_addr(65)) == 0xCAFE0001);
    CHECK(f.read32(f.layout().label_addr(66)) == 0x11223344);
    CHECK(f.read32(f.layout().label_addr(67)) == 0x11443322);
    CHECK(f.read32(f.layout().ref_addr()) == 0x77);
    CHECK(f.failures.empty());
}

TEST_CASE("an unaligned NV4097 semaphore offset is a FIFO error")
{
    Fixture f;
    auto b = f.at(0);
    b.method(m::NV4097_SET_SEMAPHORE_OFFSET, 0x14);
    b.method(m::NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE, 1);
    REQUIRE(f.submit(b) == Rsx::RunStatus::Idle); // error recovery skipped to put
    REQUIRE(f.failures.size() == 1);
    CHECK(f.failures[0].kind == FailKind::FifoError);
}

// D-587: the acquire reads its semaphore with an acquire load, which faults on ARM at an unaligned address; such an
// offset (a desync's word) is a failure and the acquire passes, as for an offset that does not translate.
TEST_CASE("an unaligned NV406E semaphore acquire offset is a failure, not a load")
{
    Fixture f;
    auto b = f.at(0);
    b.method(m::NV406E_SET_CONTEXT_DMA_SEMAPHORE, loc::kDmaSemaphoreRW);
    b.method(m::NV406E_SEMAPHORE_OFFSET, 3 * 0x10 + 2);
    b.method(m::NV406E_SEMAPHORE_ACQUIRE, 1);
    b.method(m::NV4097_SET_ALPHA_REF, 0x55);
    REQUIRE(f.submit(b) == Rsx::RunStatus::Idle);
    REQUIRE(f.failures.size() == 1);
    CHECK(f.failures[0].kind == FailKind::FifoError);
    CHECK(f.failures[0].method == m::NV406E_SEMAPHORE_ACQUIRE);
    CHECK(f.state().reg(m::NV4097_SET_ALPHA_REF) == 0x55); // the commands after it run
}

TEST_CASE("a FIFO error in a method's handler drops the commands after it up to put")
{
    // Recovery sets get to put (RPCS3 recover_fifo); the reader used to overwrite that with the next word's address and
    // count the packet's arguments down past zero, taking every later word for an argument (D-277).
    Fixture f;
    auto b = f.at(0);
    b.method(m::NV4097_SET_SEMAPHORE_OFFSET, 0x14);
    b.method(m::NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE, {1, 2}); // the second argument goes with the first
    b.method(m::NV4097_SET_BLEND_ENABLE, 1);
    b.method(m::NV4097_SET_ALPHA_REF, 0x55);
    REQUIRE(f.submit(b) == Rsx::RunStatus::Idle);
    REQUIRE(f.failures.size() == 1);
    CHECK(f.failures[0].kind == FailKind::FifoError);
    CHECK(f.state().reg(m::NV4097_SET_BLEND_ENABLE) == 0);
    CHECK(f.state().reg(m::NV4097_SET_ALPHA_REF) == 0);
    CHECK(f.rsx->get() == b.offset());
    CHECK(f.get_register() == b.offset());

    // The reader goes on with what comes next.
    auto c = f.at(b.offset());
    c.method(m::NV4097_SET_ALPHA_REF, 0x66);
    REQUIRE(f.submit(c) == Rsx::RunStatus::Idle);
    CHECK(f.state().reg(m::NV4097_SET_ALPHA_REF) == 0x66);
    CHECK(f.failures.size() == 1);
}

TEST_CASE("get and the get register stop on a blocking acquire's own word, after a hundred methods")
{
    // D-277 (a reader that published get once a stretch was tried and not kept): whatever the reader's shape, the
    // guest sees get on the acquire's word while it blocks, and at put once it has passed.
    Fixture f;
    const u32 sem = f.layout().label_addr(3);
    f.write32(sem, 0);
    auto b = f.at(0);
    for (u32 i = 0; i < 100; i++) b.method(m::NV4097_SET_ALPHA_REF, i);
    b.method(m::NV406E_SET_CONTEXT_DMA_SEMAPHORE, loc::kDmaSemaphoreRW);
    b.method(m::NV406E_SEMAPHORE_OFFSET, 3 * 0x10);
    const u32 acquire_at = b.offset() + 4; // its argument's word
    b.method(m::NV406E_SEMAPHORE_ACQUIRE, 1);
    b.method(m::NV4097_SET_ALPHA_REF, 0x1234);
    REQUIRE(f.submit(b) == Rsx::RunStatus::Blocked);
    CHECK(f.state().reg(m::NV4097_SET_ALPHA_REF) == 99);
    CHECK(f.rsx->get() == acquire_at);
    CHECK(f.get_register() == acquire_at);
    f.write32(sem, 1);
    REQUIRE(f.rsx->run_until_idle() == Rsx::RunStatus::Idle);
    CHECK(f.state().reg(m::NV4097_SET_ALPHA_REF) == 0x1234);
    CHECK(f.get_register() == b.offset());
    CHECK(f.failures.empty());
}

TEST_CASE("the RSX thread consumes the FIFO and sleeps on semaphores")
{
    Fixture f;
    const u32 label = f.layout().label_addr(70);
    f.write32(label, 0);
    f.rsx->start();
    REQUIRE(f.rsx->running());

    auto b = f.at(0);
    b.method(m::NV4097_CLEAR_SURFACE, 1);
    f.set_put(b.offset());
    f.rsx->notify();
    REQUIRE(f.rsx->wait_idle(2'000'000'000ull));
    CHECK(f.backend.clears() == std::vector<u32>{1});

    // Wait on a label another thread releases.
    b.method(m::NV406E_SET_CONTEXT_DMA_SEMAPHORE, loc::kDmaSemaphoreRW);
    b.method(m::NV406E_SEMAPHORE_OFFSET, 70 * 0x10);
    b.method(m::NV406E_SEMAPHORE_ACQUIRE, 5);
    b.method(m::NV4097_CLEAR_SURFACE, 2);
    b.method(m::NV406E_SEMAPHORE_OFFSET, 71 * 0x10);
    b.method(m::NV406E_SEMAPHORE_RELEASE, 6);
    f.set_put(b.offset());
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(f.backend.clears().size() == 1); // still blocked
    CHECK_FALSE(f.rsx->wait_idle(1'000'000ull));

    std::thread releaser([&] { f.write32(label, 5); });
    releaser.join();
    // Poll the label the RSX releases, as a PPU thread would.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (f.read32(f.layout().label_addr(71)) != 6 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    CHECK(f.read32(f.layout().label_addr(71)) == 6);
    REQUIRE(f.rsx->wait_idle(2'000'000'000ull));
    f.rsx->stop();
    CHECK(f.backend.clears() == std::vector<u32>{1, 2});
    CHECK_FALSE(f.rsx->running());
}

TEST_CASE("set_fifo_pointers from another thread is taken by the running RSX thread")
{
    Fixture f;
    f.rsx->start();
    auto b = f.at(0x2000);
    b.method(m::NV4097_CLEAR_SURFACE, 9);
    f.rsx->set_fifo_pointers(0x2000, b.offset());
    REQUIRE(f.rsx->wait_idle(2'000'000'000ull));
    f.rsx->stop();
    CHECK(f.backend.clears() == std::vector<u32>{9});
}

TEST_CASE("the running RSX thread writes get and put when it takes new FIFO pointers (D-587)")
{
    // set_fifo_pointers wrote put after flagging the reset, while the thread could already have taken the new get: it
    // then read the old put and ran from the new get through the words between, off the end of the IO map
    // (test_gcm_sys under ThreadSanitizer: "FIFO offset 0x00100000 is not mapped"). The registers change when the
    // thread takes the pointers, as on RPCS3 (RSXThread.cpp new_get_put), not while it is still in the old stream.
    Fixture f;
    std::atomic<bool> in_clear{false}, go_on{false};
    f.backend.on_clear = [&](u32 mask) {
        if (mask != 1) return;
        in_clear.store(true, std::memory_order_release);
        while (!go_on.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::microseconds(50));
    };
    auto old_stream = f.at(0);
    old_stream.method(m::NV4097_CLEAR_SURFACE, 1).method(m::NV4097_CLEAR_SURFACE, 2);
    auto fresh = f.at(0x2000);
    fresh.method(m::NV4097_CLEAR_SURFACE, 9);
    f.rsx->start();
    f.set_put(old_stream.offset());
    while (!in_clear.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::microseconds(50));
    // The game's syscall, which returns once the RSX has taken the pointers; the RSX is in the first clear meanwhile.
    std::thread game([&] { f.rsx->set_fifo_pointers(0x2000, fresh.offset()); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(f.read32(f.layout().put_addr()) == old_stream.offset());
    go_on.store(true, std::memory_order_release);
    game.join();
    CHECK(f.read32(f.layout().put_addr()) == fresh.offset());
    REQUIRE(f.rsx->wait_idle(2'000'000'000ull));
    f.rsx->stop();
    CHECK(f.get_register() == fresh.offset());
    // The second clear runs only when the syscall came after the first clear ended (a slow host).
    const std::vector<u32> clears = f.backend.clears();
    CHECK((clears == std::vector<u32>{1, 9} || clears == std::vector<u32>{1, 2, 9}));
    CHECK(f.failures.empty());
}

TEST_CASE("status() reports where the FIFO is and what it is blocked on")
{
    Fixture f;
    Rsx::Status s = f.rsx->status();
    CHECK_FALSE(s.running);
    CHECK(s.block == Rsx::BlockReason::None);
    CHECK(s.get == 0);
    CHECK(s.put == 0);
    CHECK(s.words == 0);

    // A semaphore acquire: where it looks, what the semaphore holds and what the FIFO waits for.
    const u32 label = f.layout().label_addr(64);
    f.write32(label, 0x1233);
    auto b = f.at(0);
    b.method(m::NV406E_SET_CONTEXT_DMA_SEMAPHORE, loc::kDmaSemaphoreRW);
    b.method(m::NV406E_SEMAPHORE_OFFSET, 64 * 0x10);
    const u32 acquire_at = b.offset();
    b.method(m::NV406E_SEMAPHORE_ACQUIRE, 0x1234);
    REQUIRE(f.submit(b) == Rsx::RunStatus::Blocked);
    s = f.rsx->status();
    CHECK(s.block == Rsx::BlockReason::Semaphore);
    CHECK(s.block_value == 0x1234);
    CHECK(s.semaphore_context == loc::kDmaSemaphoreRW);
    CHECK(s.semaphore_offset == 64 * 0x10);
    REQUIRE(s.semaphore_addr);
    CHECK(*s.semaphore_addr == label);
    CHECK(s.semaphore_value == 0x1233);
    CHECK(s.get == acquire_at + 4); // the acquire's argument
    CHECK(s.put == b.offset());
    CHECK(s.fifo_location == FifoLocation::Io);
    CHECK(s.words == 5); // two methods with their arguments, and the acquire's header

    // Released, the FIFO runs on to a jump to itself and parks there.
    f.write32(label, 0x1234);
    const u32 park = b.offset();
    b.jump(park);
    b.method(m::NV4097_CLEAR_SURFACE, 1);
    f.set_put(b.offset());
    REQUIRE(f.rsx->run_until_idle() == Rsx::RunStatus::Blocked);
    s = f.rsx->status();
    CHECK(s.block == Rsx::BlockReason::JumpToSelf);
    CHECK(s.get == park);
    CHECK(s.block_value == f.read32(Fixture::kCmdEa + park));

    // Once the guest patches the jump, the status no longer reports it, even before the FIFO runs again.
    f.write32(Fixture::kCmdEa + park, 0);
    CHECK(f.rsx->status().block == Rsx::BlockReason::None);
    REQUIRE(f.rsx->run_until_idle() == Rsx::RunStatus::Idle);
    s = f.rsx->status();
    CHECK(s.block == Rsx::BlockReason::None);
    CHECK(s.get == s.put);
    CHECK(f.backend.clears() == std::vector<u32>{1});
}

TEST_CASE("the running RSX never sees words older than the put that covers them (D-077)")
{
    // A writer appends commands while the RSX thread consumes them, as the PPU does. Each stretch is first filled with
    // malformed words, then overwritten with the real commands, then published with put after a release fence. With
    // put read with acquire ordering the RSX only ever sees the real commands; when the read of a command is
    // satisfied before the read of put (ARM allows it for plain loads) it can see a malformed word. Probabilistic: it
    // can only fail when that ordering is broken.
    Fixture f;
    f.rsx->start();
    constexpr u32 kLaps = 20000, kStride = 32; // 4 methods with one argument each
    for (u32 lap = 0; lap < kLaps; lap++) {
        const u32 pos = lap * kStride;
        auto poison = f.at(pos);
        for (u32 w = 0; w < kStride / 4; w++) poison.word(0x80000000);
        auto b = f.at(pos);
        for (u32 k = 0; k < 4; k++) b.method(m::NV4097_CLEAR_SURFACE, lap);
        std::atomic_thread_fence(std::memory_order_release);
        f.set_put(b.offset());
        f.rsx->notify();
    }
    REQUIRE(f.rsx->wait_idle(10'000'000'000ull));
    f.rsx->stop();
    CHECK(f.failures.empty());
    const std::vector<u32> clears = f.backend.clears();
    REQUIRE(clears.size() == 4 * kLaps);
    bool ordered = true;
    for (u32 i = 0; i < clears.size(); i++) ordered = ordered && clears[i] == i / 4;
    CHECK(ordered);
}

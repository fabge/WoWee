// The JIT write window and the Warden module's mapping must agree, in the
// configuration the client is actually built with.
//
// pthread_jit_write_protect_np answers a process without
// com.apple.security.cs.allow-jit by trapping rather than by failing, so the
// window must open only where the module image is really mapped MAP_JIT. 3.1.9
// crashed every arm64 Mac at login on exactly that gap: the window was gated on
// arm64 macOS while the mapping was gated on macOS *and* no Unicorn, and the
// release build has Unicorn. Upstream fixed it by giving both one answer,
// WOWEE_MAP_JIT, and covered it in test_jit_write.cpp.
//
// That cover does not reach this build. HAVE_UNICORN is an INTERFACE compile
// definition on wowee_common and test_jit_write links only catch2_main, so it
// compiles as though there were no Unicorn while the client it guards has one:
// on any machine with Unicorn installed it takes the "mapped MAP_JIT here" arm
// and the assertion that catches the crash never runs.
//
// This file is that assertion, in a target that does link wowee_common. It is
// separate rather than a change to upstream's file because the two mechanism
// cases there mmap MAP_JIT themselves and need the window compiled in - giving
// that whole translation unit the client's definitions turns them into a SIGBUS
// instead of a test.
//
// Nothing here maps anything. The question is only whether the two gates say
// the same thing.
#include <catch_amalgamated.hpp>

#include "game/jit_write.hpp"

using wowee::game::JitWriteWindow;

TEST_CASE("the window opens only where the image is mapped MAP_JIT", "[jit]") {
#ifdef WOWEE_MAP_JIT
    // This build maps the module PROT_EXEC|MAP_JIT, so on arm64 it must ask
    // before writing - and it needs the entitlement that makes the asking legal.
    #if defined(__aarch64__) || defined(__arm64__)
    CHECK(JitWriteWindow::required());
    #else
    CHECK_FALSE(JitWriteWindow::required());
    #endif
#else
    // No MAP_JIT page exists in this build - an emulated one copies the image
    // into Unicorn instead of running it - so there is nothing to unprotect and
    // asking would take the process out.
    CHECK_FALSE(JitWriteWindow::required());
#endif
}

TEST_CASE("a window that is not required is inert", "[jit]") {
    // The guard is still written at the call sites unconditionally, so where it
    // is not required it has to cost nothing and, above all, not call anything.
    if (JitWriteWindow::required()) {
        SUCCEED("the window is live in this build; test_jit_write covers it");
        return;
    }
    JitWriteWindow outer;
    {
        JitWriteWindow inner;
    }
    SUCCEED("constructing and nesting the window called nothing");
}

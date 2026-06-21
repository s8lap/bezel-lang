// Codegen — issue #908 (discovery sweep #6): when an event handler disposes
// ANOTHER subscription during Raise, the raise loop crashed (SIGSEGV). The loop
// called bz_event_handler_at(src, i), which returns null for a slot deactivated
// mid-raise, but the generated IR loaded the handler function pointer from that
// (null) result unconditionally → dereference of null. §24.5.
//
// THE FIX: emitEventRaise null-checks the handler returned by
// bz_event_handler_at each iteration and skips (continues) a null slot.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// The verified repro: a handler disposes a later subscription during Raise.
BEZEL_TEST(event_raise_reentrant_dispose_908, handler_disposes_later_subscription) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Src "
        "  public event Ev: int; "
        "  public method Fire(v: int) Ev.Raise(v); end Fire; "
        "end Src; "
        "class C public static method Main(): int "
        "  using var src := new Src(); "
        "  var sub2: Subscription; "
        "  var sub1 := src.Ev.Subscribe((v) => do sub2.Dispose(); end); "
        "  sub2 := src.Ev.Subscribe((v) => do var x := v; end); "
        "  src.Fire(1); "  // must not crash
        "  return 42; "
        "end Main; end C;"), 42);
}

// Control: a normal multi-handler raise still fires every handler.
BEZEL_TEST(event_raise_reentrant_dispose_908, all_handlers_fire_normally) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Acc public static sum: int := 0; end Acc; "
        "class Src public event Ev: int; public method Fire(v: int) Ev.Raise(v); end Fire; end Src; "
        "class C public static method Main(): int "
        "  using var src := new Src(); "
        "  var s1 := src.Ev.Subscribe((v) => do Acc.sum := Acc.sum + v; end); "
        "  var s2 := src.Ev.Subscribe((v) => do Acc.sum := Acc.sum + v * 10; end); "
        "  var s3 := src.Ev.Subscribe((v) => do Acc.sum := Acc.sum + v * 100; end); "
        "  src.Fire(1); "
        "  return Acc.sum; "  // 1 + 10 + 100 = 111
        "end Main; end C;"), 111);
}

// Codegen — issue #966: an INNER-scope closure capturing an OUTER owned
// (using-var) class local must not free the capture cell at the inner block's
// exit (§15.3 / §27.5 / §28.9).
//
// `do var g := () => r.V; end` boxes the outer `using var r` into a heap cell
// shared with the closure. The non-escaping closure's scope-end `boxedCellFree`
// freed that cell at the INNER block's exit — but `r`'s using-var disposal (and
// any later read of `r`) live in the OUTER block and read the value THROUGH the
// cell, so the cell was freed while still in use → use-after-free / SIGSEGV (even
// when the closure was never called).
//
// THE FIX: the over-disposal guard that suppresses `boxedCellFree` when the cell
// has a reads-through-cell disposal now searches EVERY live scope, not just the
// innermost one — so a cell whose owner (`using var`) lives in an enclosing scope
// is left alive for that owner's disposal (the 8-byte cell leaks, the sound
// trade-off; no UAF, no double-free).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
// R counts live instances (ctor +1, dtor -1); a final count of 0 means the one
// instance was disposed exactly once (no leak of the object, no double-free).
const char* kR =
    "class R public static n: int := 0; public V: int; "
    "  public constructor(v: int) R.n := R.n + 1; V := v; end constructor; "
    "  destructor R.n := R.n - 1; end destructor; end R; ";
}  // namespace

// The repro: an inner-scope closure captures the outer `using var r`; the closure
// is never called. `r` is read AND disposed after the inner block — both through
// the capture cell, which must still be alive → r.V == 55 (was SIGSEGV).
BEZEL_TEST(inner_closure_outer_capture_966, value_readable_after_inner_block) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kR +
        "class C public static method Main(): int "
        "  using var r := new R(55); "
        "  do var g: () -> int := lambda() => r.V; end; "
        "  return r.V; "  // 55
        "end Main; end C;"), 55);
}

// The outer owned local is disposed EXACTLY once (R.n returns to 0) — no leak of
// the object, no double-free, despite the inner-scope capture.
BEZEL_TEST(inner_closure_outer_capture_966, disposed_exactly_once) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kR +
        "class C public static method Main(): int "
        "  do "
        "    using var r := new R(55); "
        "    do var g: () -> int := lambda() => r.V; end; "
        "  end; "
        "  return R.n; "  // 0
        "end Main; end C;"), 0);
}

// Calling the inner closure (while r is alive, inside the inner block) works too.
BEZEL_TEST(inner_closure_outer_capture_966, inner_closure_call_works) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kR +
        "class C public static method Main(): int "
        "  using var r := new R(7); "
        "  do var g: () -> int := lambda() => r.V; var k := g(); end; "
        "  return r.V; "  // 7
        "end Main; end C;"), 7);
}

// Regression: a SAME-scope closure capturing an owned local is unaffected (the
// guard already covered the same-scope case; the value is still readable).
BEZEL_TEST(inner_closure_outer_capture_966, same_scope_capture_unaffected) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kR +
        "class C public static method Main(): int "
        "  using var r := new R(99); "
        "  var f: () -> int := lambda() => r.V; "
        "  return f(); "  // 99
        "end Main; end C;"), 99);
}

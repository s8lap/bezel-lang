// fix #2616 — §29.1 (v0.236): a sync OWNING iterator driven to exhaustion after
// the consumer abandoned a value must DISPOSE the still-owned pending value, not
// merely null the slot.
//
// §29.1's owning contract transfers ownership of a yielded value to the consumer
// only at `TakeCurrent()`. In the manual protocol a consumer may call
// `MoveNext()` and then NOT take — the value stays owned by the enumerator, in
// its `current_val` frame field. The exhaustion transition in MoveNext
// (src/codegen/LlvmBackend_Iterators.cpp, the body-runs-off-its-end path) nulled
// `current_val` defensively — so a later `TakeCurrent` raises
// InvalidOperationException — but did so WITHOUT disposing, dropping the only
// reference to a live object. The abandon-then-Dispose path (disposeYieldedVal,
// same file) already disposes; only the abandon-then-drive-to-exhaustion path
// did not. It affects BOTH owning heads: `IOwningEnumerable<T>` (pre-existing)
// and, since #2610 recognized it, `IOwningEnumerator<T>`-direct.
//
// The dominant `for var x in src` desugaring takes every value, so `current_val`
// is null at the exhaustion transition and is unaffected — the guard tests below
// pin that.
//
// ORACLE. runAotChecked links with AddressSanitizer + LeakSanitizer, so a leak
// forces a nonzero exit on its own; on top of that each program returns a value
// derived from a LIVE-OBJECT COUNTER (`R._n`, incremented by R's destructor), so
// the assertion is on EXACTLY-ONCE disposal rather than merely "does not crash".
// A double-free would trip ASan (and would also over-count `R._n`), and a leak
// under-counts it — the two failure directions are distinguished by the value.
//
// The `mutable static field _n` declaration-site warning (§22.12 rule 2 form
// (iii)) is expected and is a warning, not an error; the counter is written and
// read from one domain only, which is exactly what that warning asks for. This
// is the same counting-class idiom tests/codegen/owning_enumerator_1349_test.cpp
// already uses.
//
// KNOWN RESIDUAL, deliberately NOT asserted here (filed separately): when a
// consumer abandons a value and then calls MoveNext *again*, the next
// `yield return` OVERWRITES the non-null `current_val` without disposing the
// displaced value — a second, distinct leak site (the YieldStmt store, not the
// exhaustion transition). The programs below abandon only the LAST value, so
// they isolate the exhaustion site this fix addresses. A three-element
// abandon-two program still leaks 16 bytes on the overwrite site; see the filed
// issue.

#include "support/aot_support.hpp"   // runAotChecked
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// ASan + LeakSanitizer oracle — the point of every test in this file.
int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

// Live-object counter: R's destructor increments the static `_n`, so `R._n`
// after the enumerator is disposed is the number of R instances destroyed.
const std::string kCountR =
    "class R "
    "  public v: int := 0; "
    "  public static _n: int := 0; "
    "  public constructor(n: int) v := n; end constructor; "
    "  destructor R._n := R._n + 1; end destructor; "
    "end R; ";

}  // namespace

// §29.1: IOwningEnumerable<T> — the pre-existing head. Take R(1), then MoveNext
// to make R(2) pending and ABANDON it, then drive to exhaustion. Both objects
// must be disposed exactly once: R(1) by the consumer's `using var`, R(2) by the
// enumerator on the exhaustion transition.
//
// Without the fix R(2) leaks (LeakSanitizer: "Direct leak of 16 byte(s) …
// bz$iter$Gen$MoveNext"), so LSan forces exit 1 and the assertion fails.
BEZEL_TEST(owning_iter_exhaust_2616, enumerable_abandoned_pending_disposed_on_exhaustion) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; " + kCountR +
        "class C "
        "  public static method Gen(): IOwningEnumerable<R> "
        "    yield return new R(1); "
        "    yield return new R(2); "
        "  end Gen; "
        "  private static method Drive(): int "
        "    using var source := Gen(); "
        "    using var en := source.GetOwningEnumerator(); "
        "    var taken := 0; "
        "    if en.MoveNext() then "
        "      using var x := en.TakeCurrent(); "
        "      taken := taken + x.v; "
        "    end; "
        "    var junk := 0; "
        "    while en.MoveNext() do junk := junk + 1; end; "
        "    return taken; "
        "  end Drive; "
        "  public static method Main(): int "
        "    var t := Drive(); "
        "    return R._n * 10 + t; "   // 2 disposed, 1 taken -> 21
        "  end Main; end C;"), 21);
}

// §29.1 / #2610: the symmetric IOwningEnumerator<T>-DIRECT head, reached via
// `using var en := Gen();`. Same abandonment, same exactly-once requirement.
BEZEL_TEST(owning_iter_exhaust_2616, enumerator_direct_abandoned_pending_disposed_on_exhaustion) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; " + kCountR +
        "class C "
        "  public static method Gen(): IOwningEnumerator<R> "
        "    yield return new R(1); "
        "    yield return new R(2); "
        "  end Gen; "
        "  private static method Drive(): int "
        "    using var en := Gen(); "
        "    var taken := 0; "
        "    if en.MoveNext() then "
        "      using var x := en.TakeCurrent(); "
        "      taken := taken + x.v; "
        "    end; "
        "    var junk := 0; "
        "    while en.MoveNext() do junk := junk + 1; end; "
        "    return taken; "
        "  end Drive; "
        "  public static method Main(): int "
        "    var t := Drive(); "
        "    return R._n * 10 + t; "   // 2 disposed, 1 taken -> 21
        "  end Main; end C;"), 21);
}

// §29.1: a ZERO-element owning iterator hits the same exhaustion transition with
// `current_val` still null — the added dispose must be a no-op, not a null
// dispose. Nothing is created, so nothing is destroyed.
BEZEL_TEST(owning_iter_exhaust_2616, empty_owning_iterator_exhaustion_is_a_no_op) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; " + kCountR +
        "class C "
        "  public static method Gen(): IOwningEnumerable<R> "
        "    if false then yield return new R(1); end; "
        "  end Gen; "
        "  private static method Drive(): int "
        "    using var source := Gen(); "
        "    using var en := source.GetOwningEnumerator(); "
        "    var junk := 0; "
        "    while en.MoveNext() do junk := junk + 1; end; "
        "    return junk; "
        "  end Drive; "
        "  public static method Main(): int "
        "    var t := Drive(); "
        "    return R._n * 10 + t + 7; "   // 0 disposed, 0 iterations -> 7
        "  end Main; end C;"), 7);
}

// NO DOUBLE-FREE guard, the direction that matters most here: the dominant
// `for var x in src` desugaring takes EVERY value, so `current_val` is null at
// the exhaustion transition and the added dispose must not fire. Every value is
// disposed exactly once by the consumer — a double dispose would both trip ASan
// and push the count to 6.
BEZEL_TEST(owning_iter_exhaust_2616, for_loop_protocol_disposes_each_exactly_once) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; " + kCountR +
        "class C "
        "  public static method Gen(): IOwningEnumerable<R> "
        "    yield return new R(1); "
        "    yield return new R(2); "
        "    yield return new R(3); "
        "  end Gen; "
        "  private static method Drive(): int "
        "    var s := 0; "
        "    for var x in Gen() do s := s + x.v; end; "
        "    return s; "
        "  end Drive; "
        "  public static method Main(): int "
        "    var t := Drive(); "
        "    if R._n <> 3 then return 1; end; "
        "    return 40 + t; "   // 3 disposed exactly once, sum 6 -> 46
        "  end Main; end C;"), 46);
}

// NO DOUBLE-FREE guard, the abandon-then-DISPOSE path (the one that already
// worked, via disposeYieldedVal): abandon the pending value and dispose the
// enumerator WITHOUT driving to exhaustion. The new exhaustion dispose must not
// make this a double dispose — both sites are dispose-then-null under the same
// null guard, so whichever runs first leaves the slot null for the other.
BEZEL_TEST(owning_iter_exhaust_2616, abandon_then_dispose_without_exhaustion_still_once) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; " + kCountR +
        "class C "
        "  public static method Gen(): IOwningEnumerable<R> "
        "    yield return new R(1); "
        "    yield return new R(2); "
        "  end Gen; "
        "  private static method Drive(): int "
        "    using var source := Gen(); "
        "    using var en := source.GetOwningEnumerator(); "
        "    var junk := 0; "
        "    if en.MoveNext() then junk := junk + 1; end; "
        "    return junk; "   // pending R(1) never taken; en disposed here
        "  end Drive; "
        "  public static method Main(): int "
        "    var t := Drive(); "
        "    return R._n * 10 + t; "   // 1 disposed exactly once -> 11
        "  end Main; end C;"), 11);
}

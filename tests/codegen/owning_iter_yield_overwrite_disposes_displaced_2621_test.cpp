// fix #2621 — §29.1 (v0.236): a sync OWNING iterator's NEXT `yield return`
// must DISPOSE the still-owned value the consumer never did, not merely
// overwrite the slot — the SECOND, distinct leak site from #2616 (the
// exhaustion transition). The YieldStmt handler's store into `current_val`
// silently dropped the displaced object; this lane closes that hole.
//
// §29.1's owning contract transfers ownership of a yielded value to the
// consumer only at `TakeCurrent()`. In the manual protocol a consumer may
// call `MoveNext()` and then NOT take — the value stays owned by the
// enumerator, in its `current_val` frame field. If the consumer then calls
// `MoveNext()` again, the next `yield return` stores the new value into the
// same slot, OVERWRITING the still-owned previous value without disposing it.
// The displaced object is leaked.
//
// Scope NARROWLY: the fix only fires on the fresh-producer yield shape
// (`new X()`); the `yield return ownedLocal` / `yield return ownedParam`
// shapes have their own §29.7/§29.8 state-keyed discharge that the issue
// author flagged as a double-free hazard against a too-broad dispose. Those
// are pinned as separate, non-overlapping tests below.
//
// ORACLE. runAotChecked links with AddressSanitizer + LeakSanitizer, so a
// leak forces a nonzero exit on its own; on top of that each program returns
// a value derived from a LIVE-OBJECT COUNTER (`R._n`, incremented by R's
// destructor), so the assertion is on EXACTLY-ONCE disposal rather than
// merely "does not crash". A double-free would trip ASan (and would also
// over-count `R._n`), and a leak under-counts it — the two failure
// directions are distinguished by the value.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

const std::string kCountR =
    "class R "
    "  public v: int := 0; "
    "  public static _n: int := 0; "
    "  public constructor(n: int) v := n; end constructor; "
    "  destructor R._n := R._n + 1; end destructor; "
    "end R; ";

}  // namespace

// §29.1: IOwningEnumerable<T> — abandon TWO values in a row (the repro from
// the issue). Take R(1), advance to R(2) and ABANDON it, then advance to
// R(3) and ABANDON it. R(2) is displaced by the R(3) `yield return`; without
// the fix R(2) leaks.
//
// Counting: R(1) taken by `using var x` (1 disposed). R(2) displaced by
// R(3) yield — must be disposed at the YieldStmt store (the fix). R(3)
// disposed at the exhaustion transition (#2616). Net: 3 disposed, 1 taken.
BEZEL_TEST(owning_iter_yield_overwrite_2621, enumerable_abandoned_displaced_disposed_on_next_yield) {
    std::string program = std::string("module M; ") + kCountR +
        "class C "
        "  public static method Gen(): IOwningEnumerable<R> "
        "    yield return new R(1); "
        "    yield return new R(2); "
        "    yield return new R(3); "
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
        "    return R._n * 10 + t; "
        "  end Main; end C;";
    BEZEL_EXPECT_EQ(runChecked(program), 31);
}

// §29.1 / #2610: the symmetric IOwningEnumerator<T>-DIRECT head, reached
// via `using var en := Gen();`. Same abandonment, same exactly-once
// requirement.
BEZEL_TEST(owning_iter_yield_overwrite_2621, enumerator_direct_abandoned_displaced_disposed_on_next_yield) {
    std::string program = std::string("module M; ") + kCountR +
        "class C "
        "  public static method Gen(): IOwningEnumerator<R> "
        "    yield return new R(1); "
        "    yield return new R(2); "
        "    yield return new R(3); "
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
        "    return R._n * 10 + t; "
        "  end Main; end C;";
    BEZEL_EXPECT_EQ(runChecked(program), 31);
}

// NO DOUBLE-FREE guard: the `yield return ownedLocal` shape — the §29.7
// (#2306) state-keyed discharge path. The fresh-producer gate on the fix
// must EXCLUDE this shape — the previous current_val here came from an
// already-transferred local, the discharge of which §29.7's state-keyed
// logic owns on a separate branch — and the new dispose is the sole releaser
// for the displaced transferred value (state-keyed gates on `state <
// transferredAtState`, which is FALSE past the transfer). Exactly-once.
BEZEL_TEST(owning_iter_yield_overwrite_2621, owned_local_yield_does_not_double_dispose) {
    std::string program = std::string("module M; ") + kCountR +
        "class C "
        "  public static method Gen(): IOwningEnumerable<R> "
        "    yield return new R(1); "
        "    var L := new R(2); "
        "    yield return L; "
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
        "    return R._n * 10 + t; "
        "  end Main; end C;";
    BEZEL_EXPECT_EQ(runChecked(program), 21);
}

// NO DOUBLE-FREE guard: the `yield return ownedParam` shape — the §29.8
// (#2154) state-keyed discharge path. Same reasoning as the local case: the
// new dispose is the sole releaser for the displaced transferred-param value
// (the state-keyed `state < yieldState` gate is FALSE past the transfer).
//
// Counting: take the yielded param R(1), abandon R(2) (still in current_val),
// drive to exhaustion. R(1) disposed by consumer's using var (1 disposed).
// R(2) is the LAST current_val, disposed at the exhaustion transition (the
// #2616 path). Net: 2 disposed, 1 taken.
BEZEL_TEST(owning_iter_yield_overwrite_2621, owned_param_yield_does_not_double_dispose) {
    std::string program = std::string("module M; ") + kCountR +
        "class C "
        "  public static method Gen(owned p: R): IOwningEnumerable<R> "
        "    yield return p; "
        "    yield return new R(2); "
        "  end Gen; "
        "  private static method Drive(): int "
        "    using var source := Gen(new R(1)); "
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
        "    return R._n * 10 + t; "
        "  end Main; end C;";
    BEZEL_EXPECT_EQ(runChecked(program), 21);
}

// NO DOUBLE-FREE guard: the dominant `for var x in src` desugaring takes
// EVERY value, so `current_val` is null at every yield store and the new
// yield-store dispose must not fire (the null guard skips it). Every value
// is disposed exactly once by the consumer's `using var`.
BEZEL_TEST(owning_iter_yield_overwrite_2621, for_loop_protocol_disposes_each_exactly_once) {
    std::string program = std::string("module M; ") + kCountR +
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
        "    return 40 + t; "
        "  end Main; end C;";
    BEZEL_EXPECT_EQ(runChecked(program), 46);
}
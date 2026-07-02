// Regression tests for #1102 part C (§29.3 / §29.11): the MANUAL iterator
// protocol over a yield-method IEnumerable — `it := C.Gen().GetEnumerator();
// it.MoveNext()` — was broken in two layers:
//   (1) SEMA: `GetEnumerator()` on a vtable-backed IEnumerable<T> (a yield-
//       method result) was not typed as the builtin IEnumerator<T> sentinel
//       (only Array/OwningArray GetEnumerator was), so `var b := it.MoveNext()`
//       inferred `b` as the i32 default and `if b then …` emitted a malformed
//       `br i32` (IR-verification crash: "Branch condition is not 'i1' type!").
//   (2) CODEGEN: `GetEnumerator()` on a vtable IEnumerable was not lowered, so
//       even an explicitly-typed `var b: bool := it.MoveNext()` ran MoveNext on
//       the wrong value and returned false.
// Fix: sema types the result as IEnumerator<T> (so the #494 MoveNext()→bool
// typing fires), and codegen returns the receiver unchanged — the yield
// method's source struct IS the enumerator (vtable slot 2 = MoveNext, slot 3 =
// Current), the same identity the §30.7 GetAsyncEnumerator path returns. A
// `using var it` then disposes exactly that one struct (slot 1). Found by
// adversarial discovery sweep #8.
//
// runJit returns the FULL int from Main() (not 8-bit wrapped); values ≤ 255.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// #1102C: `var b := it.MoveNext()` (inferred) compiles (was a `br i32` IR crash)
// and MoveNext returns true for a non-empty iterator.
BEZEL_TEST(iter_manual_enumerator_1102c, movenext_inferred_bool) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Gen(): IEnumerable<int> "
        "    yield return 1; yield return 2; "
        "  end Gen; "
        "  public static method Main(): int "
        "    using var it := C.Gen().GetEnumerator(); "
        "    var b := it.MoveNext(); "
        "    if b then return 99; else return 0; end; "    // 99
        "  end Main; end C;"), 99);
}

// #1102C: an explicitly-typed `var b: bool := it.MoveNext()` returns true (was:
// compiled but MoveNext ran on the wrong value → false → 0).
BEZEL_TEST(iter_manual_enumerator_1102c, movenext_explicit_bool) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Gen(): IEnumerable<int> "
        "    yield return 1; yield return 2; "
        "  end Gen; "
        "  public static method Main(): int "
        "    using var it := C.Gen().GetEnumerator(); "
        "    var b: bool := it.MoveNext(); "
        "    if b then return 99; else return 0; end; "    // 99
        "  end Main; end C;"), 99);
}

// #1102C: a full manual drain (MoveNext + Current loop) sums the elements.
BEZEL_TEST(iter_manual_enumerator_1102c, manual_drain_sums) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Gen(): IEnumerable<int> "
        "    yield return 10; yield return 20; yield return 30; "
        "  end Gen; "
        "  public static method Main(): int "
        "    using var it := C.Gen().GetEnumerator(); "
        "    var s := 0; var guard := 0; "
        "    var more := it.MoveNext(); "
        "    while more and guard < 100 do "
        "      s := s + it.Current; "
        "      more := it.MoveNext(); "
        "      guard := guard + 1; "
        "    end; "
        "    return s; "                                   // 10+20+30 = 60
        "  end Main; end C;"), 60);
}

// #1102C: MoveNext returns false past the end (exhausted) — a manual drain of an
// empty iterator yields nothing.
BEZEL_TEST(iter_manual_enumerator_1102c, empty_iterator_movenext_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Gen(): IEnumerable<int> "
        "    yield break; "
        "  end Gen; "
        "  public static method Main(): int "
        "    using var it := C.Gen().GetEnumerator(); "
        "    var b := it.MoveNext(); "
        "    if b then return 1; else return 7; end; "     // empty → 7
        "  end Main; end C;"), 7);
}

// Regression guard: the ordinary `for var x in C.Gen()` path (which clones a
// fresh enumerator via slot 4, a different lowering) is unchanged.
BEZEL_TEST(iter_manual_enumerator_1102c, for_in_unchanged) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Gen(): IEnumerable<int> "
        "    yield return 5; yield return 7; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var s := 0; "
        "    for var x in C.Gen() do s := s + x; end; "
        "    return s; "                                   // 12
        "  end Main; end C;"), 12);
}

// Spec-derived tests for issue #952 — the call-site `ref`/`out` modifier in
// overload resolution and call-site enforcement (§22.3 / §22.6).
//
// §22.3: "ref — pass by reference. The caller writes `ref` at the call site."
// §22.6: "Overload resolution considers number, types, and the ref/out
// modifiers." Before the fix, the call-site `ref`/`out` modifier was ignored:
// `resolveMethodOverload` bailed to a first-by-arity fallback whenever any
// argument carried a modifier, and the single-candidate path never checked it.
//
// THE FIX filters overload candidates by call-site-modifier compatibility (a
// `ref` parameter is filled only by a `ref` argument; an `out`/`out owned`
// parameter only by an `out`-family argument; a plain/`owned` parameter only by
// an argument with no `ref`/`out`). This both selects the right overload and
// enforces the mandatory keyword for a single candidate.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
// Returns the program's exit code, or -999 if it failed to compile/run (used to
// assert that a call-site `ref`/`out` mismatch is REJECTED at compile time).
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// Defect 1 (WRONG_VALUE): with both `Compute(n)` and `Compute(ref n)`, the
// call-site `ref` selects the ref overload (was: the non-ref overload won →
// 5 + 100 = 105 instead of 5 * 3 = 15).
BEZEL_TEST(ref_out_overload_952, ref_arg_selects_ref_overload) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Compute(n: int): int => n + 100; "
        "  public static method Compute(ref n: int): int "
        "    n := n * 3; return n; "
        "  end Compute; "
        "  public static method Main(): int "
        "    var x := 5; "
        "    return Compute(ref x); "  // 15
        "  end Main; "
        "end C;"), 15);
}

// Defect 2 (BAD_REJECT): a non-ref call with both overloads must NOT be a false
// ambiguity — the absence of `ref` excludes the ref overload. 10 + 1 = 11.
BEZEL_TEST(ref_out_overload_952, plain_arg_excludes_ref_overload) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Transform(n: int): int => n + 1; "
        "  public static method Transform(ref n: int): int "
        "    n := n * 2; return n; "
        "  end Transform; "
        "  public static method Main(): int "
        "    var x := 10; "
        "    return Transform(x); "  // 11
        "  end Main; "
        "end C;"), 11);
}

// Defect 3 (BAD_ACCEPT): omitting the mandatory `ref` for a `ref` parameter is a
// compile error (§22.3) — was silently treated as a ref call.
BEZEL_TEST(ref_out_overload_952, missing_ref_keyword_rejected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method SetVal(ref n: int): int n := 99; return n; end SetVal; "
        "  public static method Main(): int "
        "    var x := 1; "
        "    var r := SetVal(x); "  // missing `ref` → compile error
        "    return x; "
        "  end Main; "
        "end C;"), -999);
}

// Defect 4 (BAD_ACCEPT): a superfluous `ref` for a plain parameter is a compile
// error (§22.3) — was silently accepted.
BEZEL_TEST(ref_out_overload_952, superfluous_ref_keyword_rejected) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method F(n: int): int => n + 1; "
        "  public static method Main(): int "
        "    var x := 7; "
        "    return F(ref x); "  // superfluous `ref` → compile error
        "  end Main; "
        "end C;"), -999);
}

// No over-rejection: a valid single `ref` call is accepted and mutates through
// the reference. 41 + 1 = 42.
BEZEL_TEST(ref_out_overload_952, valid_ref_call_accepted) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Inc(ref n: int): int n := n + 1; return n; end Inc; "
        "  public static method Main(): int "
        "    var x := 41; "
        "    var r := Inc(ref x); "
        "    return x; "  // 42
        "  end Main; "
        "end C;"), 42);
}

// No over-rejection: a valid `out` call with `out var` binding is accepted.
BEZEL_TEST(ref_out_overload_952, valid_out_call_accepted) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method TryGet(out n: int): bool n := 7; return true; end TryGet; "
        "  public static method Main(): int "
        "    var ok := TryGet(out var y); "
        "    return y; "  // 7
        "  end Main; "
        "end C;"), 7);
}

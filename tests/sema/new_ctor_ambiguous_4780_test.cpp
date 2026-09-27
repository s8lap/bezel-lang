// fix #4780 (§22.6 stage 8, v0.307): an ambiguous `new T(...)` constructor
// call — two candidates with identical parameter-type order after named-arg
// binding, so both are applicable with identical conversions — must emit a
// compile error. Before this fix the `Ambiguous` outcome from
// `resolveOverload` was ignored: only `Resolved` was checked, and the call
// was accepted without publishing a resolved ctor. Codegen's `ctorForArgs`
// then picked the first-in-member-order candidate silently.
//
// SPEC: §22.6 stage 8 (v0.307): "failing [a unique winner] → ambiguity error".
// The same rule applies to constructor overloads (§22.7).
//
// FAIL-WITHOUT (on e3b0bdf0): rows 1-2 fail (both compile clean and run);
// row 3 passes (single ctor, unique winner); row 4 passes (already rejected
// before this fix).

#include <cstdio>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasError(const JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

void expectRejected(const JitResult& r, const char* diag) {
    if (!hasError(r, diag)) {
        std::fprintf(stderr, "  expected diagnostic containing: %s\n", diag);
        for (const auto& d : r.diagnostics.diagnostics())
            std::fprintf(stderr, "    got: %s\n", d.message.c_str());
    }
    BEZEL_EXPECT_TRUE(hasError(r, diag));
    BEZEL_EXPECT_FALSE(r.ran);
}

} // namespace

// Row 1: `new C(a := 1, b := "x")` against ctors `(a: int, b: string)` and
// `(b: string, a: int)`. After §22.5 named-arg binding both candidates bind
// all args; the type sequence is (int, string) for both — identical
// conversions, no unique winner → §22.6 stage 8 ambiguity error.
BEZEL_TEST(new_ctor_ambiguous_4780,
           rejects_named_arg_ambiguous_spec_22_6_stage_8) {
    auto r = runAot(
        "module M; "
        "class C public constructor(a: int, b: string) end constructor; public constructor(b: string, a: int) end constructor; end C; "
        "class C2 public static method Main(): int "
        "  var x := new C(a := 1, b := \"x\"); "
        "  return 0; "
        "end Main; end C2;");
    expectRejected(r, "no overload is unambiguously better (§22.6)");
}

// Row 2: named args with SAME type for both bindings — `new C(a := 1, b := 2)`
// against `(a: int, b: int)` and `(b: int, a: int)` — same type sequence,
// identical conversions → stage 8 ambiguity.
BEZEL_TEST(new_ctor_ambiguous_4780,
           rejects_named_arg_same_type_ambiguous_spec_22_6_stage_8) {
    auto r = runAot(
        "module M; "
        "class C public constructor(a: int, b: int) end constructor; public constructor(b: int, a: int) end constructor; end C; "
        "class C2 public static method Main(): int "
        "  var x := new C(a := 1, b := 2); "
        "  return 0; "
        "end Main; end C2;");
    expectRejected(r, "no overload is unambiguously better (§22.6)");
}

// Row 3: single ctor — unique winner, no ambiguity. Must still compile.
BEZEL_TEST(new_ctor_ambiguous_4780,
           accepts_single_ctor_no_ambiguity_spec_22_6) {
    auto r = runAot(
        "module M; "
        "class C public constructor(a: int, b: string) end constructor; end C; "
        "class C2 public static method Main(): int "
        "  var x := new C(a := 1, b := \"x\"); "
        "  return 0; "
        "end Main; end C2;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Row 4: two ctors with different parameter TYPES — int/bool vs bool/int.
// After named-arg binding the type columns differ, so §22.6 stage 8 has a
// unique winner (the one with Exact at both positions). This is a control
// row: the two candidates are NOT ambiguous.
BEZEL_TEST(new_ctor_ambiguous_4780,
           accepts_non_ambiguous_overload_named_args_spec_22_6) {
    auto r = runAot(
        "module M; "
        "class C public constructor(a: int, b: bool) end constructor; public constructor(a: bool, b: int) end constructor; end C; "
        "class C2 public static method Main(): int "
        "  var x := new C(a := 1, b := true); "
        "  return 0; "
        "end Main; end C2;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// fix-forward #4780 (§22.5 / §22.6 stages 5-8, v0.307). The first #4780 fix
// compared an argument-indexed binding against a parameter-indexed type row,
// and on a mismatch handed resolveOverload columns that were not aligned. The
// three rows below are the shapes that exposed it. FAIL-WITHOUT (the
// cherry-picked d5ff73c28d alone): row 5 compiles and prints "first", row 6 is
// rejected with "no overload of `new K` is applicable", and row 7 is rejected
// as ambiguous. With the fix-forward, all three pass.

// Row 5: a 3-cycle named-argument permutation. Both candidates are exact at
// every argument, so neither is unambiguously better: §22.6 stage 8.
BEZEL_TEST(new_ctor_ambiguous_4780,
           rejects_named_arg_three_cycle_permutation_spec_22_6_stage_8) {
    auto r = runAot(
        "module M; "
        "class K "
        "  public constructor(a: int, b: string, c: bool) Console.WriteLine(\"first\"); end constructor; "
        "  public constructor(b: string, c: bool, a: int) Console.WriteLine(\"second\"); end constructor; "
        "end K; "
        "class P public static method Main(): int "
        "  var k := new K(a := 1, b := \"x\", c := true); "
        "  return 0; "
        "end Main; end P;");
    expectRejected(r, "no overload is unambiguously better (§22.6)");
}

// Row 6: bindings differ, and only the second candidate is applicable
// (`long` -> `int` is not a §5.1 implicit conversion). The call is valid and
// resolves to the second constructor (§22.6 stage 3 leaves one candidate).
BEZEL_TEST(new_ctor_ambiguous_4780,
           accepts_named_arg_differing_bindings_single_applicable_spec_22_6) {
    auto r = runAot(
        "module M; "
        "class K "
        "  public constructor(a: int, b: string) Console.WriteLine(\"first\"); end constructor; "
        "  public constructor(b: string, a: long) Console.WriteLine(\"second\"); end constructor; "
        "end K; "
        "class P public static method Main(): int "
        "  var n: long := 5; "
        "  var k := new K(a := n, b := \"x\"); "
        "  return 0; "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("second\n"));
}

// Row 7: bindings differ and both candidates are exact at every supplied
// argument, but the second fills the default `c`. §22.6's defaulted-arity
// tie-break (stage 6) prefers the candidate that fills no default, so the call
// resolves to the first constructor rather than being ambiguous.
BEZEL_TEST(new_ctor_ambiguous_4780,
           accepts_named_arg_defaulted_arity_tie_break_spec_22_6_stage_6) {
    auto r = runAot(
        "module M; "
        "class K "
        "  public constructor(a: int, b: string) Console.WriteLine(\"first\"); end constructor; "
        "  public constructor(b: string, a: int, c: int := 0) Console.WriteLine(\"second\"); end constructor; "
        "end K; "
        "class P public static method Main(): int "
        "  var k := new K(a := 1, b := \"x\"); "
        "  return 0; "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("first\n"));
}

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

// fix #4996 (§22.5 "Optional parameters and named arguments" / §22.6, v0.307):
// `new T(...)` against a LOCAL class with 2+ explicit constructors (the
// `ctors.size() > 1` arm of `ExprTC::checkNewExprArgs`,
// src/sema/ExprTypecheck_Calls.cpp) where NO candidate constructor §22.5-binds
// the call is a compile error, exactly as the single-constructor and chain-call
// (#4824) forms of the same question already are.
//
// §22.5: "Every parameter that declares no default must be bound … leaving one
// unbound … makes the candidate inapplicable ([§22.6]) … and the error is
// reported only where no candidate survives", at "every construction site in
// the language". Before this fix `checkNewExprArgs` asked that question for
// every OTHER construction shape (a single explicit constructor, a `base(...)`/
// `this(...)` chain call per #4824) but not for an overloaded `new T(...)` — the
// `ctors.size() > 1` arm did `if (decls.empty()) return;` and left the call
// silently ACCEPTED. Codegen's `ctorForArgs` then found no target and the
// #4790 backstop (src/codegen/LlvmBackend_Exprs2.cpp) reported an `internal:`
// error ("codegen selected no constructor for this `new` that sema accepted …
// — #4790") instead of this §22.5/§22.6 diagnostic. MEASURED on 404d7c51 (the
// #4824 parent): the issue's repro (row 1 below) compiled to the `internal:`
// backstop text rather than a sema error.
//
// Row 1 is the issue's own repro: neither candidate's arity matches the call's
// argument count, so no candidate could ever bind — the generic "no
// constructor … can bind these arguments" message. Row 2 has exactly one
// candidate of the call's raw parameter-list length; that candidate is
// reported by its own binder reason (an unknown named argument), the same
// text a lone-constructor `new T(...)` of the same shape gets. Row 3 is an
// accept control: an overloaded `new T(...)` where one candidate DOES bind is
// unaffected, and the constructed value is asserted.

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

void dumpDiagnostics(const JitResult& r) {
    std::fprintf(stderr, "  diagnostics (count=%zu):\n",
                 r.diagnostics.diagnostics().size());
    for (const auto& d : r.diagnostics.diagnostics())
        std::fprintf(stderr, "    [%s]\n", d.message.c_str());
}

void expectRejected(const JitResult& r, const char* diag) {
    if (!hasError(r, diag)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, diag));
    BEZEL_EXPECT_FALSE(r.ran);
}

// The issue's repro never produces the loud `internal:` #4790 backstop text —
// sema now rejects it first.
void expectNoInternalBackstop(const JitResult& r) {
    BEZEL_EXPECT_FALSE(hasError(r, "internal: codegen selected no constructor"));
}

}  // namespace

// Row 1 — the issue's repro: `new B(6)` against
// `constructor(x: int, y: int)` / `constructor(s: string, t: string, u: string)`.
// Neither candidate has arity 1, so no candidate could ever bind.
BEZEL_TEST(ctor_new_overload_no_candidate_4996,
           new_overload_no_arity_matches_rejected_spec_22_5_22_6) {
    auto r = runAot(
        "module M;\n"
        "public class B\n"
        "  public readonly property Tag: string;\n"
        "  public constructor(x: int, y: int) Tag := $\"B{x},{y}\"; "
        "end constructor;\n"
        "  public constructor(s: string, t: string, u: string) Tag := s; "
        "end constructor;\n"
        "end B;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(new B(6).Tag);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    expectRejected(r,
                   "`new B`: no constructor of `B` can bind these "
                   "arguments — each candidate leaves a required parameter "
                   "unbound, lacks a parameter the call names, or takes "
                   "fewer arguments (§22.5, §22.6)");
    expectNoInternalBackstop(r);
}

// Row 2 — exactly one candidate has the call's raw parameter-list length (2):
// `constructor(x: int, y: int)`. That candidate's own §22.5 binder reason is
// reported: `zzz` names no parameter of it.
BEZEL_TEST(ctor_new_overload_no_candidate_4996,
           new_overload_single_arity_match_unknown_name_rejected_spec_22_5) {
    auto r = runAot(
        "module M;\n"
        "public class B\n"
        "  public readonly property Tag: string;\n"
        "  public constructor(x: int, y: int) Tag := $\"B{x},{y}\"; "
        "end constructor;\n"
        "  public constructor(s: string, t: string, u: string) Tag := s; "
        "end constructor;\n"
        "end B;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(new B(zzz := 6, y := 2).Tag);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    expectRejected(r, "`new B` has no parameter named `zzz` (§22.5)");
    expectNoInternalBackstop(r);
}

// Row 3 — accept control: an overloaded `new T(...)` where one candidate DOES
// bind is unaffected; the constructed value is asserted.
BEZEL_TEST(ctor_new_overload_no_candidate_4996,
           new_overload_candidate_binds_accepted_spec_22_5) {
    auto r = runAot(
        "module M;\n"
        "public class B\n"
        "  public readonly property Tag: string;\n"
        "  public constructor(x: int, y: int) Tag := $\"B{x},{y}\"; "
        "end constructor;\n"
        "  public constructor(s: string, t: string, u: string) Tag := s; "
        "end constructor;\n"
        "end B;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(new B(6, 7).Tag);\n"
        "    Console.WriteLine(new B(\"a\", \"b\", \"c\").Tag);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    if (!r.ran) dumpDiagnostics(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "B6,7\na\n");
}

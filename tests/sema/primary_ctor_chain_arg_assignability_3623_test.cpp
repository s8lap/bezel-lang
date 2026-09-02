// Spec-derived regression test for issue #3623 (§22.5 / §5.1 / §22.10 /
// §22.8 step 0, v0.286): "this(...) chain call to a primary constructor is
// §22.5-bound but not §5.1-typechecked — mistyped argument reaches a wild
// store".
//
// spec §22.5 "Optional parameters and named arguments" (v0.286) binds
// positional and named call arguments to a callee's parameter list — for a
// `this(...)` chain call inside a convenience constructor, the callee is the
// class's primary constructor (§22.10), whose positional parameters ARE the
// positional fields (#3583 / §22.8 step 0). §5.1 "Implicit conversions"
// (v0.286) requires that every argument be ASSIGNABLE to the parameter's
// declared type. The chain path of #3583 published the §22.5 binding on
// the same side table the `new T(...)` path reads, but §5.1 ran only on the
// `new T(...)` side (`checkPositionalNewArgsFromSymbols`'s per-slot loop).
//
// WHAT THE DEFECT WAS. With `class B(n: int, s: string)` and a convenience
// `public constructor(x: int) { this("bad", 1); }`, the chain call is
// §22.5-bound to fields `n`/`s` (the binder accepted: positional, two args,
// two fields), but the bound argument types ("bad": `string` to field
// `n: int`, and `1`: `int` to field `s: string`) never reached a §5.1
// check. Codegen's `storePrimaryCtorPositionalArgs` then stored the string
// into the `int` slot (printed as `4423056`) and the int into the `string`
// slot, and `Show()` dereferenced the int as a `string` header — a
// SIGSEGV.
//
// WHAT THE FIX DOES. The §5.1 per-slot loop that
// `checkPositionalNewArgsFromSymbols` runs on a `new T(...)` is now factored
// into `ExprTC::checkPositionalArgsAssignable` and called for the chain
// site too. Both paths share one body, one diagnostic wording, one
// `ref`/`out` exact-match arm, one generic-param substitution arm. The
// chain site passes `nullptr` for the `new` target — bare non-generic-param
// slots still go through `checkAssignable` regardless, and the
// generic-substitution arm is the only thing that defers.
//
// FAIL-WITHOUT / PASS-WITH. Without the fix, `rejects_*` rows compile CLEAN
// on HEAD (no diagnostic at all); with the fix, each one reports the §5.1
// message for the swapped argument and refuses to emit. The two `accepts_*`
// rows are the false-rejection guard: the conforming `this(...)` chain
// keeps compiling (and still prints the values the chain bound), and the
// matching `new T(...)` (control) keeps compiling — both sides of the
// unification stay indistinguishable in well-typed code.

#include <cstdio>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// True iff SOME error diagnostic contains every `needle` substring. Rejection
// rows assert on the SPECIFIC §5.1 wording `checkPositionalArgsAssignable`
// produces for the `this(...)`-chain case, never on the coarse
// `hasErrors()` — `runAot` synthesizes an Error diagnostic for a runtime
// SIGSEGV/ICE too, so `hasErrors()` cannot tell a genuine reject from the
// very crash this fix removes.
bool hasAssignError(const JitResult& r, const char* descNeedle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("not implicitly convertible") == std::string::npos)
            continue;
        if (d.message.find("5.1") == std::string::npos) continue;
        if (d.message.find(descNeedle) == std::string::npos) continue;
        return true;
    }
    return false;
}

void dumpDiagnostics(const JitResult& r) {
    std::fprintf(stderr, "  diagnostics (count=%zu):\n",
                 r.diagnostics.diagnostics().size());
    for (const auto& d : r.diagnostics.diagnostics())
        std::fprintf(stderr, "    [%s] %s\n",
                     d.severity == bezel::Severity::Error ? "E" : "W",
                     d.message.c_str());
    if (!r.stdoutText.empty())
        std::fprintf(stderr, "  stdout: %s\n", r.stdoutText.c_str());
}

constexpr const char* kChainDesc =
    "this(...)` chain to the primary constructor of `B";
constexpr const char* kNewDesc = "`new B`";

}  // namespace

// ---------------------------------------------------------------------------
// Rejection rows. Each pins a SPECIFIC §5.1 diagnostic — the same wording
// `checkAssignable` produces for a `new T(...)` mismatch, just addressed
// to the chain site.
// ---------------------------------------------------------------------------

// #3623 / §22.5 / §5.1 (v0.286): the issue's measured repro, in a complete
// program. The chain call passes `"bad"` (string) into the `n: int` slot
// and `1` (int) into the `s: string` slot. On `be7f2f7` this compiled
// CLEAN, ran to SIGSEGV when `Show()` dereferenced the int as a string
// header. With the fix, both slots get a §5.1 diagnostic — the same
// `checkAssignable` text `new B("bad", 1)` already gets.
BEZEL_TEST(primary_ctor_chain_arg_assignability_3623,
           rejects_this_chain_string_arg_into_int_slot_spec_5_1) {
    JitResult r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public constructor(x: int) "
        "    this(\"bad\", 1); "
        "  end constructor; "
        "end B; "
        "class C public static method Main(): int "
        "  using var b := new B(42); "
        "  return 0; "
        "end Main; end C;");
    if (!hasAssignError(r, kChainDesc)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasAssignError(r, kChainDesc));
}

// #3623 / §22.5 / §5.1 (v0.286): the symmetric case — `int` into `string`
// slot. The slot pair is the same, so the §5.1 diagnostic is symmetric too;
// pinning BOTH directions guards against a fix that only catches one
// direction (e.g. an accidental `int → string` allow-rule that the chain
// path silently inherits).
BEZEL_TEST(primary_ctor_chain_arg_assignability_3623,
           rejects_this_chain_int_arg_into_string_slot_spec_5_1) {
    JitResult r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public constructor(x: int) "
        "    this(99, 7); "
        "  end constructor; "
        "end B; "
        "class C public static method Main(): int "
        "  using var b := new B(42); "
        "  return 0; "
        "end Main; end C;");
    if (!hasAssignError(r, kChainDesc)) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasAssignError(r, kChainDesc));
}

// ---------------------------------------------------------------------------
// False-rejection guards. Each pins a VALUE — the chain call still produces
// the object §22.8 step 0 describes, and the control `new T(...)` row still
// produces its expected output. Without these the fix would over-reject.
// ---------------------------------------------------------------------------

// #3623 / §22.10 / §22.8 step 0 (v0.286): the CONFORMING `this(...)` chain.
// The convenience constructor binds its argument to the primary's slot via
// §22.5, the §5.1 check above accepts (both arguments are string/int and
// match `s: string` / `n: int`), and the object's `Show()` prints what
// §22.8 step 0 says it should — the chain-bound values, NOT the
// new-site arguments the pre-#3583 blind store would have paired. This is
// the row that proves the §5.1 check didn't accidentally over-reject the
// good case.
BEZEL_TEST(primary_ctor_chain_arg_assignability_3623,
           accepts_this_chain_correct_types_spec_5_1) {
    JitResult r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public constructor(x: int) "
        "    this(x, \"chained\"); "
        "  end constructor; "
        "  public method Show(): string => s + \":\" + n.ToString(); "
        "end B; "
        "class C public static method Main(): int "
        "  using var b := new B(42); "
        "  Console.WriteLine(b.Show()); "
        "  return 0; "
        "end Main; end C;");
    if (r.stdoutText != "chained:42\n") dumpDiagnostics(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("chained:42\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// #3623 / §22.5 / §5.1 (v0.286): the CONTROL row — the `new T(...)` twin
// of the same call. The whole point of the unification is that the chain
// path runs the SAME rule the new path already runs; if the new path is
// broken (a regression in `checkPositionalArgsAssignable`'s extraction),
// this row fails before the chain row can be trusted. The `new` site
// already rejected mismatched types — verified separately by
// `arrow_body_return_convertibility_1774_test.cpp`'s §5.1 cases — and
// this row pins the ACCEPTED twin of that reject so a fix that flips the
// answer fails here too.
BEZEL_TEST(primary_ctor_chain_arg_assignability_3623,
           accepts_new_T_correct_types_control_spec_5_1) {
    JitResult r = runAot(
        "module M; "
        "class B(n: int, s: string) "
        "  public method Show(): string => s + \":\" + n.ToString(); "
        "end B; "
        "class C public static method Main(): int "
        "  using var b := new B(42, \"newpath\"); "
        "  Console.WriteLine(b.Show()); "
        "  return 0; "
        "end Main; end C;");
    if (r.stdoutText != "newpath:42\n") dumpDiagnostics(r);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("newpath:42\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Codegen — issue #5032: inside a generic method template `where T: IShapeN`,
// (1) an interpolated string calling a T-typed receiver's member
// (`$"{v.Area()}xy"`) raised an unconditional internal error #1386, and
// (2) a `case` type-pattern arm over a T-typed scrutinee
// (`case v of var q: SqN => q.Area() + 10, _ => 1`) failed LLVM IR
// verification ("block has no terminator") with NO diagnostic at all. Both
// reject/crash a program that is legal per §14.1/§14.2 (v0.307): a generic
// method constrained on an interface is entitled to call the interface's
// members on its type parameter, in any expression position, including
// inside a string-interpolation hole and inside a `case` arm body.
//
// Found by the R21 #5009 review; MEASURED single-module, so it is
// pre-existing and not caused by #5009. Probes: /var/tmp/bezel-scratch/
// r21-rev5009c/p/loc_head_Interp.bzl, loc_head_CaseT.bzl.
//
// ROOT CAUSE (both). Every generic method is emitted TWICE (issue #490):
// once as a DEAD "unspecialized template" body, where T is erased to a
// non-pointer placeholder LLVM type (i32) and never actually called, and
// once per call site as a SPECIALIZED instantiation with the real concrete
// type and real vtable dispatch — only the specialized body's output is
// ever observed. Inside the dead template body, `v.Area()` hits
// `emitMethodCall`'s #484 non-pointer-receiver guard
// (LlvmBackend_Calls2.cpp) and returns null, which is a well-established,
// EXPECTED shape (`emittingGenericTemplateBody_`'s dead code): the sibling
// value-demanding sites (`return expr;` #2491, a local's initializer #2434)
// already have a three-outcome dispatch for exactly this — degrade to a
// placeholder value and report a Warning, not an Error, per issue #2924's
// "four-way-mirror" discipline — but the interpolation-part site and the
// case-arm-body site had never been given it (issue #2966 named three
// OTHER residual sites; these two are new, distinct ones).
//
// FIX (interpolation, LlvmBackend_Exprs.cpp): the interpolation-part `!ev`
// arm gets the same three-outcome dispatch as #2434/#2491: (a) unit-typed
// part, exempt (pre-existing); (b) a member call on a built-in receiver
// that produced no value — a genuine defect — Error; (c) everything else,
// including the dead-template-body case, degrades to a placeholder value
// via `lowerTypeRef` and reports only a Warning.
//
// FIX (case arm, LlvmBackend_Exprs2.cpp): `lowerCaseExpr`'s per-arm `!v`
// handling already had two void-only exemptions (fix #4469) but fell to an
// unconditional `return nullptr` when the case has a CONCRETE result type
// and the arm body still failed to lower — exactly this shape. Unlike the
// interpolation site, the case expression has ALREADY created live basic
// blocks (`bodyBB`, `nextBB`, the shared `case.end`) before this point, so
// bailing out with `return nullptr` left them without a terminator — an IR
// verifier abort with NO diagnostic, not even the #1386 ICE text. The fix
// synthesizes a placeholder value (`llvm::Constant::getNullValue(resultTy)`)
// in BOTH the Error and Warning branches and falls through to the existing
// PHI/CreateBr(endBB) machinery, so every block gets terminated regardless
// of diagnostic severity — mirroring the lambda-body precedent for this
// exact shape (this file's own #1255 fix).
//
// Neither fix touches `emitMethodCall` (LlvmBackend_Calls2.cpp) or its
// #5009-review gates: both are at the CALLER side of the #484 guard's null
// return, in the two sibling files.
//
// BEHAVIOUR-FLIP GREP: grepped every "#1386" occurrence (~150+ hits) across
// tests/ before this fix. No existing test asserted the old unconditional
// Error / IR-verifier-abort behaviour for a null interpolation part or a
// null case-arm body specifically in the dead-unspecialized-generic-
// template-body shape; the closest hit
// (generic_bare_typeparam_tostring_interp_2503_test.cpp) asserts a
// DIFFERENT, earlier-reported diagnostic (#2479, non-auto-hashable choice
// ToString) that fires before this dispatch runs, so `hasErrors()` is
// already true there and this fix's own diagnostic is correctly suppressed
// — that test's assertions are unaffected. No test needed updating.
//
// FAIL-WITHOUT: measured and recorded in the commit message (mechanical
// `git checkout -- <file>` + rebuild, one file at a time).
//
// NOTE on what this file can and cannot observe: `runAot`'s `JitResult`
// only carries a real `DiagnosticEngine` on a compile REJECT — on a
// successful compile (`CompileOutcome::Ok`), the captured compiler stderr
// is deliberately DISCARDED before it ever reaches `r.diagnostics`
// (tests/support/aot_support.hpp, ~line 1500: "the captured compiler
// stderr is DISCARDED on CompileOutcome::Ok"), because a link-cache hit
// never re-runs the compiler at all, so there is nothing to parse back
// consistently. Every program below compiles and RUNS successfully, so
// `r.diagnostics` is structurally empty regardless of severity; a
// diagnostic-text assertion here would either fail unconditionally (the
// Warning IS emitted, per the direct `bezelc` probes recorded in the
// commit message, but is unobservable through this harness) or pass
// vacuously (always true for the non-generic controls, which never reach
// the new code path, and for any other successful compile). Neither is a
// real check, so this file asserts only the runtime VALUE, which already
// proves the fix: a hard Error would make `r.ran` false and `out()` return
// "<did-not-run>", so a correct value proves the dead template body's
// null-value degrades to a placeholder and nothing worse.
//
// §14.1 / §14.2 (constraints and member access through them) / §19.3
// ("Type parameters as pattern types": a bare `T`/`var x: T` pattern tests
// the substituted type argument) / §19.5 (a typed binding pattern matches
// only non-null values — irrelevant to non-null runtime dispatch here, but
// governs the arm's semantics), v0.307.

#include "support/aot_support.hpp"   // runAot — AOT: the shipping oracle
#include "support/codegen_support.hpp"  // JitResult
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

std::string out(const std::string& src) {
    auto r = runAot(src);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0)
        return "<exit " + std::to_string(r.exitCode) + ">" + r.stdoutText;
    return r.stdoutText;
}

}  // namespace

// ── The issue's own repro (interpolation half), verbatim: `IShapeN`/`SqN`/
//    `GN.Interp<T>` are the exact names and bodies from
//    loc_head_Interp.bzl, trimmed of the ~9 sibling probe methods the R21
//    review used to isolate this site (IfaceLocal/IfExpr/IsNarrow/CaseE/
//    Loop/AsNarrow/Nullable/Holder/InterpT), which are not part of this bug.
//    FAILS before the fix (#1386 Error, does not compile); PASSES after
//    ("7xy"). ─────────────────────────────────────────────────────────────
//
// per non-author review (2026-09-26): the original oracle here asserted
// `s.Length + 100` (103), which does not DISCRIMINATE — "7xy" and a
// hypothetical wrongly-degraded "0xy" are both length 3, so a derived-length
// oracle passes on either string. Asserting the raw interpolated text
// instead strengthens the oracle: it can only read "7xy" if the dead
// template body's real, specialized clone actually dispatched `v.Area()`
// to `SqN.Area()` and produced 7, not the placeholder 0 a degrade would
// substitute in the (never-taken) dead-body path.

BEZEL_TEST(generic_template_tparam_member_5032,
           issue_repro_interpolated_t_member_call_spec_14_2) {
    const std::string program =
        "module Interp5032;\n"
        "public interface IShapeN\n"
        "  method Area(): int;\n"
        "end IShapeN;\n"
        "public class SqN: IShapeN\n"
        "  public constructor() end constructor;\n"
        "  public method Area(): int return 7; end Area;\n"
        "end SqN;\n"
        "public static class GN\n"
        "  public static method Interp<T>(v: T): string where T: IShapeN\n"
        "    return $\"{v.Area()}xy\";\n"
        "  end Interp;\n"
        "end GN;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(GN.Interp(new SqN()));\n"
        "  end Main;\n"
        "end Program;\n";
    // A hard Error here would leave r.ran false and out() == "<did-not-run>";
    // getting the raw interpolated string "7xy" (not a derived length that
    // would collide with a wrongly-degraded "0xy") proves the dead template
    // body's `v.Area()` degraded to a placeholder rather than aborting, AND
    // that the real, specialized clone actually dispatched to `SqN.Area()`.
    BEZEL_EXPECT_EQ(out(program), std::string("7xy\n"));
}

// ── The issue's own repro (case half), verbatim: `GN.CaseT<T>` is the
//    exact name and body from loc_head_CaseT.bzl, same trim. FAILS before
//    the fix (IR verifier abort: "Basic Block ... does not have
//    terminator! label %case.end" — a crash, not even a diagnostic);
//    PASSES after (17 = 7 + 10, the `var q: SqN` arm). ────────────────────

BEZEL_TEST(generic_template_tparam_member_5032,
           issue_repro_case_type_pattern_on_t_spec_19_3) {
    const std::string program =
        "module Case5032;\n"
        "public interface IShapeN\n"
        "  method Area(): int;\n"
        "end IShapeN;\n"
        "public class SqN: IShapeN\n"
        "  public constructor() end constructor;\n"
        "  public method Area(): int return 7; end Area;\n"
        "end SqN;\n"
        "public static class GN\n"
        "  public static method CaseT<T>(v: T): int where T: IShapeN\n"
        "    return (case v of var q: SqN => q.Area() + 10, _ => 1);\n"
        "  end CaseT;\n"
        "end GN;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(GN.CaseT(new SqN()));\n"
        "  end Main;\n"
        "end Program;\n";
    // Pre-fix this was an IR verifier abort with NO diagnostic at all
    // (see the file header); getting the correct value proves both that
    // the block gets a terminator and that the `var q: SqN` arm's
    // `q.Area() + 10` result is the one that actually ran.
    BEZEL_EXPECT_EQ(out(program), std::string("17\n"));
}

// ── Non-generic TWIN control (interpolation): the identical interpolation
//    shape over a CONCRETE `SqN` parameter — no `where T:` clause, so no
//    dual-emission (#490) and no dead template body exists at all. Confirms
//    the fix's new dispatch branch is correctly UNREACHED here: the value
//    is right AND no "#1386" diagnostic of any severity fires. ────────────

BEZEL_TEST(generic_template_tparam_member_5032,
           non_generic_twin_interpolation_control_spec_14_2) {
    const std::string program =
        "module InterpCtl5032;\n"
        "public interface IShapeN\n"
        "  method Area(): int;\n"
        "end IShapeN;\n"
        "public class SqN: IShapeN\n"
        "  public constructor() end constructor;\n"
        "  public method Area(): int return 7; end Area;\n"
        "end SqN;\n"
        "public static class GN\n"
        "  public static method InterpConcrete(v: SqN): int\n"
        "    var s := $\"{v.Area()}xy\";\n"
        "    return s.Length + 100;\n"
        "  end InterpConcrete;\n"
        "end GN;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(GN.InterpConcrete(new SqN()));\n"
        "  end Main;\n"
        "end Program;\n";
    BEZEL_EXPECT_EQ(out(program), std::string("103\n"));
}

// ── Non-generic TWIN control (case): same, for the type-pattern `case`
//    arm over a CONCRETE `SqN` scrutinee. ───────────────────────────────

BEZEL_TEST(generic_template_tparam_member_5032,
           non_generic_twin_case_type_pattern_control_spec_19_3) {
    const std::string program =
        "module CaseCtl5032;\n"
        "public interface IShapeN\n"
        "  method Area(): int;\n"
        "end IShapeN;\n"
        "public class SqN: IShapeN\n"
        "  public constructor() end constructor;\n"
        "  public method Area(): int return 7; end Area;\n"
        "end SqN;\n"
        "public static class GN\n"
        "  public static method CaseTConcrete(v: SqN): int\n"
        "    return (case v of var q: SqN => q.Area() + 10, _ => 1);\n"
        "  end CaseTConcrete;\n"
        "end GN;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(GN.CaseTConcrete(new SqN()));\n"
        "  end Main;\n"
        "end Program;\n";
    BEZEL_EXPECT_EQ(out(program), std::string("17\n"));
}

// ── Extra variant: TWO T-member calls in one interpolation hole
//    (`$"{v.Area()}-{v.Area()}"`), plus a `case` whose type-pattern arm is
//    exercised at ONE instantiation (T = SqN, matches `var q: SqN`) and
//    whose fallthrough `_` arm is exercised at ANOTHER (T = TriN, a second
//    IShapeN implementer that is not a SqN, so the typed arm never
//    matches and the `_` arm's own T-member call — `v.Area() + 1000` —
//    runs instead). Both the interpolation hole and the `_` arm's body
//    exercise a T-member call inside the SAME dead template body as the
//    matched arm, at the same two sites this fix covers. §14.1/§14.2/
//    §19.6 (exhaustiveness — `_` covers the residual case; not statically
//    provable exhaustive without it since `IShapeN` is open). ────────────

BEZEL_TEST(generic_template_tparam_member_5032,
           extra_two_member_interpolation_and_case_fallthrough_spec_14_2) {
    const std::string program =
        "module Extra5032;\n"
        "public interface IShapeN\n"
        "  method Area(): int;\n"
        "end IShapeN;\n"
        "public class SqN: IShapeN\n"
        "  public constructor() end constructor;\n"
        "  public method Area(): int return 7; end Area;\n"
        "end SqN;\n"
        "public class TriN: IShapeN\n"
        "  public constructor() end constructor;\n"
        "  public method Area(): int return 99; end Area;\n"
        "end TriN;\n"
        "public static class GN\n"
        "  public static method InterpTwo<T>(v: T): string where T: "
        "IShapeN\n"
        "    return $\"{v.Area()}-{v.Area()}\";\n"
        "  end InterpTwo;\n"
        "  public static method CaseFallthrough<T>(v: T): int where T: "
        "IShapeN\n"
        "    return (case v of var q: SqN => q.Area() + 10, _ => "
        "v.Area() + 1000);\n"
        "  end CaseFallthrough;\n"
        "end GN;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(GN.InterpTwo(new SqN()));\n"
        "    Console.WriteLine(GN.CaseFallthrough(new SqN()));\n"
        "    Console.WriteLine(GN.CaseFallthrough(new TriN()));\n"
        "  end Main;\n"
        "end Program;\n";
    BEZEL_EXPECT_EQ(out(program), std::string("7-7\n17\n1099\n"));
}

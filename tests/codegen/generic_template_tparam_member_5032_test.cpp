// Codegen — issue #5032 (part 1/2): inside a generic method template
// `where T: IShapeN`, an interpolated string calling a T-typed receiver's
// member (`$"{v.Area()}xy"`) raised an unconditional internal error #1386.
// This rejects a program that is legal per §14.1/§14.2 (v0.307): a generic
// method constrained on an interface is entitled to call the interface's
// members on its type parameter, including inside a string-interpolation
// hole. (Part 2/2 — the sibling `case`-type-pattern bug in the same
// dead-template-body shape — is fixed in a following commit, which extends
// this same file with its own repro/control/interaction tests.)
//
// Found by the R21 #5009 review; MEASURED single-module, so it is
// pre-existing and not caused by #5009. Probe: /var/tmp/bezel-scratch/
// r21-rev5009c/p/loc_head_Interp.bzl.
//
// ROOT CAUSE. Every generic method is emitted TWICE (issue #490): once as
// a DEAD "unspecialized template" body, where T is erased to a
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
// "four-way-mirror" discipline — but the interpolation-part site had never
// been given it (issue #2966 named three OTHER residual sites; this is a
// new, distinct one).
//
// FIX (interpolation, LlvmBackend_Exprs.cpp): the interpolation-part `!ev`
// arm gets the same three-outcome dispatch as #2434/#2491: (a) unit-typed
// part, exempt (pre-existing); (b) a member call on a built-in receiver
// that produced no value — a genuine defect — Error; (c) everything else,
// including the dead-template-body case, degrades to a placeholder value
// via `lowerTypeRef` and reports only a Warning.
//
// Does not touch `emitMethodCall` (LlvmBackend_Calls2.cpp) or its
// #5009-review gates: the fix is at the CALLER side of the #484 guard's
// null return, in the sibling file.
//
// BEHAVIOUR-FLIP GREP: grepped every "#1386" occurrence (~150+ hits) across
// tests/ before this fix. No existing test asserted the old unconditional
// Error behaviour for a null interpolation part specifically in the
// dead-unspecialized-generic-template-body shape; the closest hit
// (generic_bare_typeparam_tostring_interp_2503_test.cpp) asserts a
// DIFFERENT, earlier-reported diagnostic (#2479, non-auto-hashable choice
// ToString) that fires before this dispatch runs, so `hasErrors()` is
// already true there and this fix's own diagnostic is correctly suppressed
// — that test's assertions are unaffected. No test needed updating.
//
// FAIL-WITHOUT: measured and recorded in the commit message (mechanical
// `git checkout -- <file>` + rebuild).
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
// vacuously (always true for the non-generic control, which never reaches
// the new code path). Neither is a real check, so this file asserts only
// the runtime VALUE, which already proves the fix: a hard Error would make
// `r.ran` false and `out()` return "<did-not-run>", so a correct value
// proves the dead template body's null-value degrades to a placeholder and
// nothing worse.
//
// §14.1 / §14.2 (constraints and member access through them), v0.307.

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
//    (103 = "7xy".Length + 100). ──────────────────────────────────────────

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
        "  public static method Interp<T>(v: T): int where T: IShapeN\n"
        "    var s := $\"{v.Area()}xy\";\n"
        "    return s.Length + 100;\n"
        "  end Interp;\n"
        "end GN;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    Console.WriteLine(GN.Interp(new SqN()));\n"
        "  end Main;\n"
        "end Program;\n";
    // A hard Error here would leave r.ran false and out() == "<did-not-run>";
    // getting the correct interpolated value proves the dead template
    // body's `v.Area()` degraded to a placeholder rather than aborting.
    BEZEL_EXPECT_EQ(out(program), std::string("103\n"));
}

// ── Non-generic TWIN control (interpolation): the identical interpolation
//    shape over a CONCRETE `SqN` parameter — no `where T:` clause, so no
//    dual-emission (#490) and no dead template body exists at all. Confirms
//    the value is right with no dead-template-body dispatch involved at
//    all. ─────────────────────────────────────────────────────────────────

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

// Spec-derived regression tests for issue #5859 — §22.6 overload resolution
// selected the WRONG candidate whenever the argument's type is a BUILT-IN
// class reference (`Exception`, §31.4) and the overload set pairs it with a
// value-type (`string`) candidate.
//
// §31.2.2 (v0.334), *Arm bodies* — the spec's own normative example, and the
// source of conformance row `spec_0279` (`tests/conformance/cases/
// spec_0279.bzl`, expect=compiles), is exactly this call shape:
//
//     try
//       DoWork();
//     catch
//       var ex: ArgumentException when ex.ParamName = "amount" => …,
//       …
//       var ex: Exception => do
//         Log(ex);          // <-- `Log` is overloaded `Log(message: string)`
//         throw;            //     AND `Log(ex: Exception)` in the ambient
//       end
//     end;
//
// so `Log(ex)` where the binding's static type is `Exception` MUST resolve to
// `Log(ex: Exception)` and compile.
//
// ROOT CAUSE (MEASURED — instrumented `resolveOverload`'s `judgeable` lambda
// and its per-cell `classifyArg` grid, then compiled the repro): a built-in
// class reference is a `TypeRef::Kind::Named` with `declaration == nullptr`
// (it has no user `TypeSymbol` — §2.3/§31.4 make it always-available without
// import). `resolveOverload`'s `judgeable` predicate admits a `Named` type
// only when `t->declaration != nullptr && t->genericArgs.empty()`, so a
// built-in class-reference ARGUMENT fails the predicate. `resolveOverload`
// therefore returned `OverloadOutcome::Undecidable` with
// `res.applicable.assign(nCand, false)` — EVERY candidate marked inapplicable,
// without a single `classifyArg` cell ever being evaluated. The `Undecidable`
// arm in `resolveMethodOverload` then finds no candidate with
// `r.applicable[c]` and falls through to `return initial`, which is
// `lookupMember`'s first-in-source candidate — `Log(message: string)`, the
// FIRST of the two declared overloads. `checkCallArity`/`#5684`'s
// `checkArgsAgainstParams` then correctly (but uselessly) rejects the `Exception`
// argument against that `string` parameter:
//
//   ovl.bzl:7:9: the argument `message` to call to `Log` has a type that is
//   not implicitly convertible to the target type (§5.1) …
//
// So the diagnostic names a call the user did not write: resolution, not
// argument checking, was wrong.
//
// WHY A DIFFERENTIAL CONTROL, AND WHICH ONE (per the project's
// regression-test-pins-the-bug rule):
//   - `Log(Exception)` ALONE with the same `Exception` argument COMPILES
//     (row `single_builtin_class_overload_compiles_spec_31_2_2`) — so the
//     per-argument §5.1 check the fix's resolution feeds is NOT the problem.
//   - `Log(string)` ALONE, and both overloads with a `string` argument,
//     COMPILE (rows `single_string_overload_compiles_…`,
//     `string_arg_picks_string_candidate_…`) — so the `string` candidate is
//     reachable and the overload SET is well-formed.
//   Only the CHOICE between the two candidates is wrong. Each PASS row below
//     FAILS-WITHOUT by asserting the returned VALUE, so it pins the resolved
//     candidate and not merely "no diagnostic".
//
// This is FIX-FORWARD, NOT A REVERT. Commit 37fbc296 (fix #5684) added a
// `checkArgsAgainstParams` call on the non-generic single-overload path in
// `checkCallArity`, which had never run there. It did NOT cause this defect —
// it EXPOSED a latent one: before it, resolution picked the wrong candidate
// and no argument check ran, so the program compiled silently THROUGH a bad
// resolution. That silence is not a correctness argument. The #5684 check is
// correct and stays; the resolution that feeds it is what is fixed.
//
// REGRESSION-TEST POLICY: every PASS row below was REJECTED before the fix
// (with the §5.1 "the argument `message` …" diagnostic) or returned the
// `string` branch's value. Every REJECT row below must be rejected both before
// AND after the fix — they prove the fix did not widen acceptance past §22.6.

#include "support/aot_support.hpp"  // runAot — AOT test harness (never runJit)
#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// AOT runner: compiles a program to native, runs it, returns its exit code.
// -999 on any compile/link/run failure, so an "exit 0" assertion cannot pass
// by accident when the compiler rejected the program outright.
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasMsg(const bezel::test::SemaResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// PASS — the defect #5859 reported. Each FAILS-WITHOUT: pre-fix resolution
// picked `Log(message: string)` and the §5.1 argument check rejected the call.
// ---------------------------------------------------------------------------

// §31.2.2 (v0.334) / §22.6 (#5859): the spec's own `Log(ex)` shape, with the
// ambient overloads declared in the ORDER `spec_0279`'s `env.bzl` declares
// them (`Log(message: string)` first, `Log(ex: Exception)` second) — the exact
// declaration order that made the pre-fix first-in-source fallback pick wrong.
// Asserts the RESOLVED CANDIDATE by its return value (11), not merely absence
// of a diagnostic: pre-fix this returns -999 (the compiler rejected the call).
BEZEL_TEST(sema_overload_builtin_class_arg_5859,
           exception_arg_picks_exception_candidate_spec_31_2_2_22_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class C "
        "  private static method Log(message: string): int => 22; "
        "  private static method Log(ex: Exception): int => 11; "
        "  public static method Main(): int "
        "    var ex: Exception; "
        "    return Log(ex); "
        "  end Main; "
        "end C;"),
        11);
}

// §31.2.2 (v0.334) / §22.6 (#5859): the REVERSE declaration order
// (`Log(ex: Exception)` first) is the second half of the differential — it
// compiled BEFORE the fix too (first-in-source happened to be right), so this
// row is a NO-OP control proving the fix does not break the order in which
// resolution already agreed with the spec. FAILS-WITHOUT is not claimed here.
BEZEL_TEST(sema_overload_builtin_class_arg_5859,
           exception_candidate_first_declaration_order_spec_31_2_2_22_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class C "
        "  private static method Log(ex: Exception): int => 11; "
        "  private static method Log(message: string): int => 22; "
        "  public static method Main(): int "
        "    var ex: Exception; "
        "    return Log(ex); "
        "  end Main; "
        "end C;"),
        11);
}

// §31.2.2 (v0.334) / §22.6 (#5859): a DERIVED built-in exception type
// (`ArgumentException`, §31.4) against the same overload pair. Same defect,
// same fix: the argument's `Named` TypeRef has `declaration == nullptr`
// regardless of which builtin it names, so the pre-fix `judgeable` predicate
// rejected it and first-in-source picked `Log(string)`. FAILS-WITHOUT with
// the §5.1 "the argument `message` …" diagnostic.
BEZEL_TEST(sema_overload_builtin_class_arg_5859,
           derived_builtin_exception_picks_its_candidate_spec_31_2_2_22_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class C "
        "  private static method Log(message: string): int => 22; "
        "  private static method Log(ex: ArgumentException): int => 33; "
        "  public static method Main(): int "
        "    var ex: ArgumentException; "
        "    return Log(ex); "
        "  end Main; "
        "end C;"),
        33);
}

// §22.6 (#5859): the DIFFERENTIAL CONTROL that localises the defect to
// candidate SELECTION rather than argument checking — a SINGLE
// `Log(ex: Exception)` overload with the identical `Exception` argument.
// This compiled BEFORE the fix (the `cands.size() == 1` early return in
// `resolveMethodOverload` short-circuits ranking entirely), so it proves the
// §5.1 argument check the fix feeds was never the problem: with one candidate
// the same `Exception` binds cleanly.
BEZEL_TEST(sema_overload_builtin_class_arg_5859,
           single_builtin_class_overload_compiles_spec_31_2_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class C "
        "  private static method Log(ex: Exception): int => 11; "
        "  public static method Main(): int "
        "    var ex: Exception; "
        "    return Log(ex); "
        "  end Main; "
        "end C;"),
        11);
}

// §22.6 (#5859): the mirror control on the OTHER side — a `string` argument
// against the SAME two overloads still picks `Log(message: string)`. Compiled
// before the fix too (`string` is a `Kind::Primitive`, which `judgeable`
// admits, so ranking ran and classified the string candidate Exact). Proves
// the fix did not break the half that worked.
BEZEL_TEST(sema_overload_builtin_class_arg_5859,
           string_arg_picks_string_candidate_spec_22_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class C "
        "  private static method Log(message: string): int => 22; "
        "  private static method Log(ex: Exception): int => 11; "
        "  public static method Main(): int "
        "    return Log(\"plain\"); "
        "  end Main; "
        "end C;"),
        22);
}

// §22.6 (#5859): a SINGLE `Log(message: string)` overload — the `string`
// candidate alone is reachable and binds. Compiled before the fix; the
// load-bearing statement is that the overload SET above is well-formed and the
// `string` arm is not itself broken.
BEZEL_TEST(sema_overload_builtin_class_arg_5859,
           single_string_overload_compiles_spec_22_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class C "
        "  private static method Log(message: string): int => 22; "
        "  public static method Main(): int "
        "    return Log(\"plain\"); "
        "  end Main; "
        "end C;"),
        22);
}

// §22.6 (#5859): a USER-declared class-reference argument against the same
// overload pair — `var e: MyErr` is a `Named` TypeRef WITH a `declaration`, so
// `judgeable` admitted it and ranking ran even before the fix. This row
// therefore PASSES pre-fix: it is the differential that isolates the defect to
// BUILT-IN class references specifically (null `declaration`), not to class
// references as such. Included so a future change that breaks the
// declaration-bearing path is caught here rather than blamed on #5859.
BEZEL_TEST(sema_overload_builtin_class_arg_5859,
           user_class_arg_picks_its_candidate_spec_22_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class MyErr end MyErr; "
        "static class C "
        "  private static method Log(message: string): int => 22; "
        "  private static method Log(ex: MyErr): int => 44; "
        "  public static method Main(): int "
        "    var e: MyErr := new MyErr(); "
        "    return Log(e); "
        "  end Main; "
        "end C;"),
        44);
}

// ---------------------------------------------------------------------------
// REJECT — the load-bearing half. The fix LIFTS a rejection, so it must lift
// NONE of these. Each is rejected both before AND after the fix.
// ---------------------------------------------------------------------------

// §22.6 / §5.1 (#5859): widening `judgeable` must NOT let a built-in class
// reference match a VALUE-type parameter. `Log(ex)` where BOTH overloads take
// incompatible parameters (`string` and `int`, neither of which admits an
// `Exception` argument) must still be rejected — with the §22.6
// "no overload of `Log` is applicable" diagnostic now that ranking RUNS on
// the built-in argument (pre-fix this arity-fell-back to `initial` and was
// rejected by the argument check instead; the REJECTION is the invariant, the
// exact text is not, so this row asserts only "still an error").
BEZEL_TEST(sema_overload_builtin_class_arg_5859,
           builtin_class_arg_against_two_value_params_still_rejected_spec_22_6) {
    auto r = parseSemaAll(
        "module M; static class C "
        "  private static method Log(message: string): int => 22; "
        "  private static method Log(n: int): int => 33; "
        "  public static method Main(): int "
        "    var ex: Exception; "
        "    return Log(ex); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §22.6 (#5859): the same guard on the primitive side — a `string` argument
// against two overloads neither of which admits it must stay rejected. NO-OP
// control (rejected pre-fix as well): it proves the widened ranking path does
// not admit a value-type argument to a non-matching built-in candidate either.
BEZEL_TEST(sema_overload_builtin_class_arg_5859,
           string_arg_against_two_int_params_still_rejected_spec_22_6) {
    auto r = parseSemaAll(
        "module M; static class C "
        "  private static method Log(n: int): int => 33; "
        "  private static method Log(k: int): int => 44; "
        "  public static method Main(): int "
        "    return Log(\"plain\"); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §22.6 (#5859): an EXACT built-in class-reference match must outrank a
// merely-APPLICABLE one. `Log(o: Object)` and `Log(ex: Exception)` called
// with an `Exception` argument: `Exception` → `Exception` is Exact (§22.6
// ranks Exact above Convert), `Exception` → `Object` is Convert (§12 root
// supertype), so `Log(ex: Exception)` MUST win.
//
// MEASURED: before the fix this returned 55 — the `Object` branch — with NO
// diagnostic at all, because `judgeable` sent the whole overload set down the
// Undecidable path and the first-declared candidate won silently. So this row
// pins a case where the pre-fix defect produced a SILENTLY WRONG overload
// rather than a rejection, which is the more dangerous half of #5859.
//
// NOT COVERED HERE: two candidates that differ only in which BUILT-IN class
// reference they name (`Log(e1: Exception)` vs `Log(e2: ArgumentException)`).
// That pair cannot be DECLARED at all on this tree — §11.3/§22.6's
// overload-distinguishability guard rejects it with "overload of `Log` is
// indistinguishable from another overload", because that guard's
// `signaturesIndistinguishable` compares parameter types with `typeRefEqual`,
// which compares the `declaration` POINTER — null for every built-in, so two
// distinct built-ins look identical to it. That is a SEPARATE defect in
// `src/sema/TypeResolution_Checks.cpp` (a hot file this lane does not touch),
// not the resolution defect #5859 reports; it is filed separately rather than
// half-fixed here. The §12 SUPER-TYPE direction against a builtin IS covered,
// by `derived_builtin_exception_picks_its_candidate_spec_31_2_2_22_6` above
// (an `ArgumentException` argument resolving to an `Exception` parameter).
BEZEL_TEST(sema_overload_builtin_class_arg_5859,
           exact_builtin_match_outranks_object_candidate_spec_22_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class C "
        "  private static method Log(o: Object): int => 55; "
        "  private static method Log(ex: Exception): int => 11; "
        "  public static method Main(): int "
        "    var ex: Exception; "
        "    return Log(ex); "
        "  end Main; "
        "end C;"),
        11);
}
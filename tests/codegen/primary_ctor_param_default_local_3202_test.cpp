// fix #3202 (§22.10 "primary constructors" / §22.5 "Optional parameters and
// named arguments" / §22.8 step 0; spec v0.254) — a §22.10 primary-constructor
// parameter may carry a DEFAULT (`class DefBox(A: int, B: int := 9)`), and the
// local-twin half of #3202 makes the codegen fill the trailing defaulted slots
// from the parameter's `:= Expr` instead of leaving the entire object
// calloc-zero when the call is short.
//
// WHAT THE DEFECT WAS. `LlvmBackend_Exprs2.cpp::storePrimaryCtorPositionalArgs`
// (the §22.8 step 0 caller-side storage helper, the same one #3141/#3162
// landed) required `ne.args.size() == posFields.size()`. When a call was
// SHORT of the parameter list (`new DefBox(1)` against the class above), the
// guard short-circuited the entire store loop — so EVERY slot was left at its
// calloc zero, not just the trailing defaulted one. `Total()` then read `0`
// instead of `10`. The diagnostic was silent — no reject, no warning, just a
// wrong value (measured at `0a3e6c0`, the lane-base commit, by the local repro
// in `scratch/repro_local_3202.bzl`).
//
// THE FIX (local half). The codegen now (a) accepts
// `ne.args.size() <= posFields.size()` and (b) per slot, evaluates the
// parameter's `ast::Param::defaultValue` Expr* under the enclosing scope's
// substitution when the supplied argument is absent. The local Expr* is the
// source-level `:= 9` literal — no separate fold path needed; just
// `lowerExpr` it with the same `#597` / `#2367` bracket the supplied arm uses.
//
// ⚠ THE LOCAL HALF IS PART OF A TWO-HALF FIX. The cross-module half
// (`ModuleMetadata` carriage of the default text + importer-side fold +
// `checkPositionalNewArgsFromSymbols`'s hasDefault predicate) is pinned by
// `tests/module/xmod_primary_ctor_param_default_3202_test.cpp`; this file pins
// only the local-half defect, so the local twin continues to read the right
// value even if the cross-module half is ever reverted.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// T1 — §22.10 / §22.5: a §22.10 class with a defaulted primary parameter,
// supplied with the full argument list. Pre-fix this was the one shape that
// worked (it satisfied `args.size() == posFields.size()`); pin it so a
// regression to the strict-equality gate is caught loud rather than silent.
BEZEL_TEST(primary_ctor_param_default_local_3202,
           full_arg_list_still_writes_every_slot_spec_22_10) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "public class DefBox(A: int, B: int := 9)"
        "  public method Total(): int return A + B; end Total;"
        "end DefBox;"
        "class C public static method Main(): int"
        "  using var d := new DefBox(1, 2);"
        "  if d.Total() <> 3 then return 1; end;"
        "  return 0;"
        "end Main; end C;"), 0);
}

// T2 — the LOCAL HALF of #3202: the under-supplied call that pre-fix read 0.
// Two default-bearing parameters: `B` defaulted. The defect is that
// `ne.args.size() == posFields.size()` was false (1 != 2), so the store
// loop was skipped and `A` was zero too — `Total()` read 0 instead of 10.
// After #3202, `A` is stored from the supplied arg, `B` is filled from the
// default `:= 9`, and the read is 10.
BEZEL_TEST(primary_ctor_param_default_local_3202,
           under_supplied_call_fills_default_slot_spec_22_5) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "public class DefBox(A: int, B: int := 9)"
        "  public method Total(): int return A + B; end Total;"
        "end DefBox;"
        "class C public static method Main(): int"
        "  using var d := new DefBox(1);"
        "  if d.Total() <> 10 then return 2; end;"
        "  return 0;"
        "end Main; end C;"), 0);
}

// T2b — both supplied but via the under-supplied arm: a class with EVERY
// parameter defaulted. The pre-fix defect read 0 (no args supplied →
// `args.size() (0) != posFields.size() (2)`, store loop skipped). After #3202
// both slots are filled from defaults.
BEZEL_TEST(primary_ctor_param_default_local_3202,
           all_defaults_all_defaulted_parameters_spec_22_5) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "public class DefBox(A: int := 5, B: int := 9)"
        "  public method Total(): int return A + B; end Total;"
        "end DefBox;"
        "class C public static method Main(): int"
        "  using var d := new DefBox();"
        "  if d.Total() <> 14 then return 3; end;"
        "  return 0;"
        "end Main; end C;"), 0);
}

// T3 — §22.5 "Every parameter after the first defaulted parameter must
// itself have a default" — a class with a defaulted parameter followed by a
// required parameter is a PARSE-/DECL-level error. Pin the runtime arm of
// the rule: a class with a defaulted middle parameter and a required tail
// still stores the supplied tail correctly (the gap-default contract is
// checked at declaration, not construction).
BEZEL_TEST(primary_ctor_param_default_local_3202,
           required_after_defaulted_still_resolves_at_construction_spec_22_5) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "public class Triple(A: int, B: int := 7, C: int)"
        "  public method Total(): int return A + B + C; end Total;"
        "end Triple;"
        "class C public static method Main(): int"
        "  using var d := new Triple(1, 2, 3);"
        "  if d.Total() <> 6 then return 4; end;"
        "  return 0;"
        "end Main; end C;"), 0);
}

// T4 — pre-empt: a fix that only handled the under-supplied case by reading
// the default into `A` rather than `B` (a typo, swapping which slot reads
// from `posParam->defaultValue`) would PASS T2 — `A` is supplied (1), `B` is
// defaulted (9), and the buggy fix that fills `A`'s slot from `B`'s default
// would still answer 1 + 9 = 10. The pre-empt that catches it is to use
// ASYMMETRIC defaults so a swap-fix would be distinguishable from the
// correct fix: with `A := 100, B := 9` and a call supplying only `1`, the
// correct answer is `1 + 9 = 10`; a swap-fix that fills `B`'s slot from
// `A`'s default would answer `1 + 100 = 101`.
BEZEL_TEST(primary_ctor_param_default_local_3202,
           each_under_supplied_slot_reads_its_own_default_spec_22_5) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "public class Asym(A: int := 100, B: int := 9)"
        "  public method Total(): int return A + B; end Total;"
        "end Asym;"
        "class C public static method Main(): int"
        "  using var d := new Asym(1);"
        "  if d.Total() <> 10 then return 5; end;"
        "  return 0;"
        "end Main; end C;"), 0);
}

// T5 — gap, deferred to #3227. The spec-prescribed case — a primary-ctor
// parameter's default expression referencing an EARLIER parameter on the
// SAME list (`B := A + 1`) — requires a separate capture-iff-in-default
// sema pass: the `posParamBodyRef` capture-iff-used rule
// (`src/sema/ExprTypecheck_Exprs.cpp:1097`) only fires when the parameter is
// referenced in a field initializer or a method body, not when referenced
// only inside a later parameter's default. The codegen half of #3202
// evaluates the default with `currentThis_ = obj`, expecting the
// referenced earlier param to resolve through `implicitFieldAddr` to a
// captured field slot — but the capture never happens, so the identifier
// resolves to nothing and the read errors. Tracked at
// https://github.com/s8lap/bezel-lang/issues/3227. Re-add the case here
// when #3227 closes.

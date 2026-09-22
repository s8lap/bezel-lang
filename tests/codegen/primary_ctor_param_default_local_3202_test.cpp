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

// T3 — every supplied slot overrides its default, including the slots AFTER
// the first defaulted one. A construction that supplies all three must read
// back all three supplied values, not a mix of supplied and defaulted.
//
// ⚠ UPDATED BY fix #4494, NOT weakened — the reasoning, because the diff
// looks like a fixture swap. This row used to declare
// `public class Triple(A: int, B: int := 7, C: int)` and its own comment said
// that shape "is a PARSE-/DECL-level error", pinning the runtime arm only
// "(the gap-default contract is checked at declaration, not construction)".
// That parenthesis was a statement about a GAP: §22.5 *Defaults and
// modifiers* says "Every parameter after the first defaulted parameter must
// itself have a default", and nothing enforced it, so the illegal
// declaration compiled. #4494 enforces it, so the old fixture is now a
// compile error and this row could not have kept its old text.
//
// The oracle is UNCHANGED (`run(...) == 0`, with the same distinguishing
// return code 4 and the same expected total of 6) and the runtime property is
// unchanged; only the declaration was made legal, by giving `C` a default it
// never takes here. The REJECTION of the old shape is pinned in its own right
// by tests/sema/param_default_ordering_4494_test.cpp, so nothing that used to
// be covered here is now uncovered.
BEZEL_TEST(primary_ctor_param_default_local_3202,
           every_supplied_slot_overrides_its_default_spec_22_5) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "public class Triple(A: int, B: int := 7, C: int := 11)"
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

// T5 — fix #3227 (§22.10 / §22.5 "Permitted default-value expressions" /
// §22.8 step 0; spec v0.254): a §22.10 primary-ctor parameter's DEFAULT
// expression may reference an EARLIER parameter on the SAME list — the
// spec example is `class W(A: int, B: int := A + 1)`. The earlier
// parameter is read by the default's evaluation (which runs against the
// partially-constructed object), so it must be captured as a real field
// slot the default can dereference — `posParamBodyRef=true`. Pre-fix,
// the defaultValue walk set `inFieldInitializer_=true` for the entire
// visit, so the §22.10 capture-iff-used rule set `posParamInitRef=true`
// (init-position consumption) instead of `posParamBodyRef=true` (capture)
// for the EARLIER parameter when it appeared ONLY inside the LATER
// parameter's default. The earlier parameter's slot was then either
// absent (storage predicate `isInstanceFieldStorageSymbol` returned false
// for it) or, on the `owned` arm, captured-but-not-disposed
// (`isCapturedOwnedPrimaryParam` rejected the slot because of
// `posParamInitRef`, leaking the owned value — #3205 family). The fix:
// a separate `inPrimaryCtorDefault_` flag gates the §22.10 rule so the
// default visit takes the body-position arm (`posParamBodyRef=true`)
// while keeping `inFieldInitializer_=true` for the §22.8 instance-
// member gate. The codegen path is unchanged — `storePrimaryCtorPositionalArgs`
// already evaluated defaults with `currentThis_=obj` (line ~2807,
// #3202's local-twin) and `implicitFieldAddr` reads the captured slot.
BEZEL_TEST(primary_ctor_param_default_local_3202,
           default_reads_earlier_param_on_same_list_spec_22_5) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "public class W(A: int, B: int := A + 1)"
        "  public method Total(): int return A + B; end Total;"
        "end W;"
        "class C public static method Main(): int"
        "  using var w := new W(10);"
        "  if w.Total() <> 21 then return 1; end;"
        "  return 0;"
        "end Main; end C;"), 0);
}

// T5b — the §22.5 "Permitted default-value expressions" shape, with the
// EARLIER parameter also referenced in a method body (so the spec's
// ordinary capture path also fires). The default reads `A`, the body
// reads `A` AND `B`. After #3227 the default visit sets
// `posParamBodyRef=true` for A (it would already have been true via the
// body's `return A + B`), but the test exists to pin the dual path:
// A must read correctly via the captured slot, and B must read the
// default-derived value (10 + 1 = 11) via the captured slot. A fix that
// captured ONLY through the body path (silently dropping the default
// visit's contribution) would still pass T5 if the body happened to read
// A, but T5b pins the defaultValue-only path with the same body shape.
BEZEL_TEST(primary_ctor_param_default_local_3202,
           default_reads_earlier_param_with_body_reference_spec_22_5) {
    BEZEL_EXPECT_EQ(run(
        "module M;"
        "public class W(A: int, B: int := A + 1)"
        "  public method GetA(): int return A; end GetA;"
        "  public method GetB(): int return B; end GetB;"
        "end W;"
        "class C public static method Main(): int"
        "  using var w := new W(10);"
        "  if w.GetA() <> 10 then return 2; end;"
        "  if w.GetB() <> 11 then return 3; end;"
        "  return 0;"
        "end Main; end C;"), 0);
}

// T5c — #3227's §22.10 OWNED case. `owned A: Res` referenced ONLY in a
// later parameter's default (`B := A.v`) — A is NEVER read in any method
// body — was pre-fix treated as init-consumed (`posParamInitRef=true`)
// because the defaultValue walk set `inFieldInitializer_=true` for the
// entire visit, so `isCapturedOwnedPrimaryParam` (`!posParamInitRef`
// arm) returned false and `emitOwnedFieldDisposals` skipped A's slot —
// the owned `Res` leaked (LSan-measurable #3205 family). After #3227,
// A's defaultValue-only visit takes the body arm and the disposal walker
// picks up the slot. The test exercises the path: an UNDER-SUPPLIED
// call (`new W(new Res(10))`) forces B's default evaluation, the default
// reads `A.v` through the captured slot, and `w`'s destructor must run
// the owned-field cascade to dispose `A`. A successful run under
// `runAotChecked` (= runAot + LeakSanitizer) means no leak on either
// Res. A pre-fix #3227 run reports an LSan leak on the Res object
// (r.ran goes false). // per §22.10, v0.254
BEZEL_TEST(primary_ctor_param_default_local_3202,
           owned_earlier_param_referenced_only_in_default_no_leak_spec_22_10) {
    auto r = bezel::test::runAotChecked(
        "module M;"
        "class Res "
        "  public v: int := 0; "
        "  public constructor(x: int) v := x; end constructor; "
        "end Res; "
        "public class W(owned A: Res, B: int := A.v) "
        "  public method GetB(): int return B; end GetB; "
        "end W; "
        "class C public static method Main(): int "
        "  using var w := new W(new Res(10)); "
        "  if w.GetB() <> 10 then return 1; end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(0, r.exitCode);
}

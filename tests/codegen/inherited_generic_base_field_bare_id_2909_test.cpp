// fix #2909 / §12 "Inheritance and Dispatch" / §14.1 "Generic declarations" /
// §23.1 "Auto-properties and fields" / §22.8 "Field initializers", v0.240.
//
// THE DEFECT. A GENERIC class that reads an INHERITED field declared on a
// generic BASE via a BARE identifier (implicit `this.field`) from one of its
// own methods lowers the field at the T→i32 ERASED PLACEHOLDER layout even
// when the base's argument is fully concrete. Any use that needs the field's
// real type then emits malformed IR and the compile aborts with an internal
// "IR verification failed" rather than a sourced diagnostic:
//
//     GEP base pointer is not a vector or a vector of pointers
//       %19 = getelementptr inbounds nuw %bz.string, i32 %18, 0, 1
//     Load operand must be a pointer.
//       %str.len = load i64, i32 %19, align 4
//
// The QUALIFIED twin (`c.bv.Length` from Main, where `c: Coll<int>`) already
// compiles and runs — that path is `fieldAddr`, which receives the
// receiver's REAL TypeRef and resolves the base-class substitution through
// it. The defect is exclusively in the bare-identifier path
// (`implicitFieldAddr`, LlvmBackend_Layout.cpp ~4740), which synthesizes a
// no-generic-args receiver TypeRef for the enclosing class and asks
// `computeAncestorGenericSubst` to derive the inherited class's params.
//
// THE ROOT CAUSE. In the SHARED TEMPLATE body of the generic enclosing class
// (e.g. `Coll<U>`), `genericSubstSemaTy_` is empty — U has no concrete
// binding yet, the template body is dead code that only has to verify. The
// `computeAncestorGenericSubst` seed step (LlvmBackend_Calls2.cpp ~8933,
// added by #2889 for the monomorphized-body case) bails with `return false`
// the moment ANY of the enclosing class's params is missing from
// `genericSubstSemaTy_`. For `class Coll<U> : Base<string>` U is missing —
// but the base clause `Base<string>` does NOT reference U (it is fully
// concrete), so the walk could have proceeded: the chain goes Coll → Base,
// the base ref's arg is the concrete `string`, and `curArgs[T]` becomes
// `string` regardless of U. The strict bail refused the whole walk, the
// substitution was never installed, and `classStruct(Base)` used the T→i32
// erased placeholder layout. The shared template body still has to verify
// (it is emitted unconditionally whether or not any vtable slot ever points
// at it), so the verifier abort is what the user sees.
//
// THE FIX (NARROW). Loosen the seed step to leave unbound params unbound
// rather than bailing, and add a CONCRETENESS check at the end that rejects
// any binding that is still a bare type-parameter ref. The two changes are
// tightly coupled: relaxing the seed alone would let bare-param-ref bindings
// slip through to `lowerTypeRef` (which produces the i32 placeholder for
// them — the original bug, just deferred to a different site), and the
// end-check alone would not help because every binding that needs the
// relaxed seed would arrive at the end as a bare ref and be rejected
// without ever producing a substitution. The chain walk is unchanged: bare
// refs that have no entry in `curArgs` already stay bare, so an
// unreferenced enclosing-class param is harmless. The "never install a
// substitution we cannot prove" discipline of #2840/#2860/#2873/#2882 is
// preserved — every key in the returned map is bound to a CONCRETE
// TypeRef or the whole call returns false.
//
// SCOPE NOTES:
//   * `fieldAddr` (the qualified-access twin) is UNCHANGED: it passes a real
//     receiver TypeRef with its own generic args, so the seed step already
//     follows the non-seed branch and resolves the chain concretely. T1
//     below covers that shape so the relaxation does not regress the
//     working path.
//   * The fix is in the SHARED helper used by every synthesized-receiver
//     caller; the other callers (`emitLocalBaseFieldInit`,
//     `emitSpecializedImplicitBaseCtor`, `emitImportedBaseFieldInit`,
//     `ancestorGenericSubstAdditions`, `instanceStorageTypes`) all run
//     under an active substitution that already binds every enclosing-class
//     param they reference, so the relaxed seed produces the same maps they
//     produced before. The #2889 / #2897 / #2873 / #2908 test files cover
//     each of those paths separately and exercise them end-to-end here.
//   * Multi-level inheritance where the base arg is an UNRESOLVED bare
//     type-parameter ref at every chain link (`D<T> : M<T> : B<T>`) does
//     NOT benefit from the relaxed seed: every chain link asks for the
//     open T, the walk produces `curArgs[T] = bare T ref`, and the end
//     check rejects it correctly — the template body is dead code anyway,
//     so the right direction is to leave it alone (T3).
//   * The §2759 residual property-shape (#2944 T4) is independent of this
//     fix — that gating is in `ensurePropertyAccessorFn`, not in
//     `implicitFieldAddr`/`computeAncestorGenericSubst`, and is unchanged.
//
// Every case runs end to end under generated-code AddressSanitizer /
// LeakSanitizer and asserts the VALUE, so a fix that only makes the verifier
// happy is not evidence the value is right.

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// #3733 toolchain-SKIP burn-down. This file's 8 checked rows used to
// guard on a local `wasSkipped` predicate whose whole body read the literal
// "SKIP" out of the result's diagnostics, and then RETURNED from the row on
// it. That is an ABSORBING site by
// scripts/check_toolchain_skip_surface.py's own definition, and it is a silent
// vacuous pass: on a box whose clang cannot link the ASan binary, all
// 8 rows returned WITHOUT asserting and the suite reported a clean PASS
// having compiled and run nothing. MEASURED on the pre-change tree with the
// harness's own knob (`BEZEL_AOT_FORCE_NO_TOOLCHAIN=1`): every row green.
//
// Every one of those guards becomes the sanctioned LOUD form,
// `BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(...))`
// (tests/support/aot_result_asserts.hpp), in the same statement position: the
// skip is now LOUD — the row FAILS — instead of being absorbed by a guard
// that reports a clean pass. The lenient legs' assertions additionally
// now RUN UNCONDITIONALLY rather than being skipped, which is the other half of
// the strengthening. Every substantive assertion in every row (r.ran and the
// exitCode pins) stays exactly where it was.

}  // namespace

// ===========================================================================
// T0 — THE ISSUE'S OWN REPRO (verbatim). §12 / §14.1 / §23.1 / §22.8.
// Pre-fix: raw IR-verifier abort on the bare-identifier `bv` read inside
// `Coll<U>.OwnRead` — the SHARED template body, active subst empty, but the
// base clause `Base<string>` does NOT reference U so the relaxed seed is
// allowed to proceed. Post-fix: 6 + 41 + 3 + 4 == 54.
// ===========================================================================
BEZEL_TEST(inherited_generic_base_field_bare_id_2909,
           issue_repro_spec_12_14_1_23_1) {
    auto r = runAotChecked(
        "module H; "
        "class Cell public tag: int := 3; end Cell; "
        "class Base<T> "
        "  public bv: T; "
        "  public owned cell: Cell := new Cell(); "
        "  public ba: int := 41; "
        "end Base; "
        "class Coll<U> : Base<string> "
        "  public dv: U; "
        "  public da: int := 6; "
        "  public method OwnRead(): int "
        "    return da + ba + cell.tag + bv.Length; "
        "  end OwnRead; "
        "end Coll; "
        "class C public static method Main(): int "
        "  using var c := new Coll<int>(); "
        "  c.bv := \"abcd\"; "
        "  return c.OwnRead(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 54);
}

// ===========================================================================
// T1 — THE QUALIFIED TWIN STAYS GREEN. `fieldAddr` (the qualified-access
// path, LlvmBackend_Layout.cpp ~5042) has always resolved this shape via the
// receiver's REAL TypeRef; the relaxed seed must not regress it. Reads
// `c.bv.Length` from Main (where `c: Coll<int>`) — the receiver's static
// type drives the non-seed branch of `computeAncestorGenericSubst`. The
// runtime reaches the same memory as the bare path on `Main`'s
// `c.OwnRead()` call, so the value is the same (54).
//
// (NB: `this.bv` from inside a derived method is a SEPARATE shape — that
// dispatches through the same `implicitFieldAddr` path as bare `bv`, and
// was broken pre-fix just like bare `bv`. The qualified-vs-bare
// distinction is about which TypeRef drives the seed step, not about
// which parser form the user wrote.)
// ===========================================================================
BEZEL_TEST(inherited_generic_base_field_bare_id_2909,
           qualified_twin_unaffected_spec_23_1) {
    auto r = runAotChecked(
        "module H; "
        "class Cell public tag: int := 3; end Cell; "
        "class Base<T> "
        "  public bv: T; "
        "  public owned cell: Cell := new Cell(); "
        "  public ba: int := 41; "
        "end Base; "
        "class Coll<U> : Base<string> "
        "  public dv: U; "
        "  public da: int := 6; "
        "  public method OwnRead(): int "
        "    return da + ba + cell.tag + bv.Length; "
        "  end OwnRead; "
        "end Coll; "
        "class C public static method Main(): int "
        "  using var c := new Coll<int>(); "
        "  c.bv := \"abcd\"; "
        "  return c.OwnRead() + c.bv.Length + c.da; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 64);
}

// ===========================================================================
// T2 — TWO-PARAMETER ENCLOSING CLASS WITH A CONCRETE BASE ARG.
// `class Coll<U,V> : Base<int>` reads `Base<T>.bv: T` (T = int, concrete).
// The base clause does not reference U or V, so neither needs to be in the
// active substitution. Reads `bv` bare from a method body, returns `bv`
// directly. Expected: 17.
// ===========================================================================
BEZEL_TEST(inherited_generic_base_field_bare_id_2909,
           two_param_enclosing_concrete_base_spec_14_1) {
    auto r = runAotChecked(
        "module H; "
        "class Base<T> "
        "  public bv: T; "
        "end Base; "
        "class Coll<U,V> : Base<int> "
        "  public method ReadBv(): int "
        "    return bv; "
        "  end ReadBv; "
        "end Coll; "
        "class C public static method Main(): int "
        "  using var c := new Coll<string, bool>(); "
        "  c.bv := 17; "
        "  return c.ReadBv(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 17);
}

// ===========================================================================
// T3 — MULTI-LEVEL GENERIC CHAIN WHERE EVERY LINK PASSES THE BARE PARAM.
// `class D<T> : M<T> : B<int>`. The chain goes D → M → B: from D, base ref
// M<T> has bare T (active subst empty in template body, so unresolved);
// from M, base ref B<int> has concrete int. The walk produces:
//   * D → M: T_M = bare T_D ref (unresolved, but doesn't matter because M
//     doesn't use it — M only declares mv: T which the method body doesn't
//     touch).
//   * M → B: T_B = int (concrete). Resolved.
// `classStruct(B)` under T → int makes `bv: int`. The bare `bv` read in
// D's method body returns the i32 it was stored as. Expected: 7.
//
// This is the case that distinguishes the LOOSENED seed from a stronger
// but narrower fix — the chain has both a bare-param-ref hop AND a
// concrete hop, and the end check is what makes the loosened seed safe
// (without it, the bare T_D ref would survive at T_M's level and the
// substitution would be installed with a bare ref that `lowerTypeRef`
// would silently turn into the i32 placeholder).
// ===========================================================================
BEZEL_TEST(inherited_generic_base_field_bare_id_2909,
           multi_level_concrete_leaf_spec_12_14_1) {
    auto r = runAotChecked(
        "module H; "
        "class B<T> "
        "  public bv: T; "
        "end B; "
        "class M<T> "
        "  public mv: T; "
        "end M; "
        "class D<T> : M<T> "
        "  public method ReadBv(): int "
        "    using var b := new B<int>(); "
        "    b.bv := 7; "
        "    return b.bv; "
        "  end ReadBv; "
        "end D; "
        "class C public static method Main(): int "
        "  using var d := new D<int>(); "
        "  return d.ReadBv(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ===========================================================================
// T4 — A GENERIC CLASS WITH A NON-GENERIC BASE READING AN INHERITED FIELD.
// `class Coll<U> : Base { ... }` reading `ba: int` bare. The base is
// NON-generic, so `implicitFieldAddr`'s outer guard
// (`!ms->parent->genericParams.empty()`) does NOT fire and
// `computeAncestorGenericSubst` is never called — the substitution is
// empty, `classStruct(Base)` builds the concrete layout directly. Pins
// the non-generic-base read so the relaxed seed doesn't accidentally
// widen `implicitFieldAddr`'s guard. Expected: 41.
// ===========================================================================
BEZEL_TEST(inherited_generic_base_field_bare_id_2909,
           non_generic_base_unchanged_spec_12_23_1) {
    auto r = runAotChecked(
        "module H; "
        "class Base public ba: int := 41; end Base; "
        "class Coll<U> : Base "
        "  public method Read(): int "
        "    return ba; "
        "  end Read; "
        "end Coll; "
        "class C public static method Main(): int "
        "  using var c := new Coll<int>(); "
        "  return c.Read(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 41);
}

// ===========================================================================
// T5 — OWN-CLASS BARE READ (non-inherited) STILL WORKS.
// `class Coll<U> { dv: U }` reads `dv` bare. `dv` is declared on `Coll`
// itself, not inherited; the substitution is empty in the template body.
// At runtime the SPEC body (per U = int instantiation) reads `dv` as i32.
// Pins the non-inherited path; the relaxation must not disturb the
// own-field shape. Expected: 99.
//
// #4768 (§5.1 / §3.6 rule 2, v0.307): this row originally declared
// `ReadDV(): int` and returned the `U`-typed field from it. A generic body
// type-checks under the weakest assumption its constraints establish, and
// §5.1 converts a type-parameter value only to `Object` and to its
// constraint bounds, so `U -> int` has no conversion: that return is now
// rejected at the declaration. The row keeps its subject (the bare read
// of an own generic field) in the spec-valid spelling: `ReadDV(): U`,
// with the value stored from `Main` at the concrete `U = int`.
// ===========================================================================
BEZEL_TEST(inherited_generic_base_field_bare_id_2909,
           own_class_bare_read_unchanged_spec_23_1) {
    auto r = runAotChecked(
        "module H; "
        "class Coll<U> "
        "  public dv: U; "
        "  public method ReadDV(): U "
        "    return dv; "
        "  end ReadDV; "
        "end Coll; "
        "class C public static method Main(): int "
        "  using var c := new Coll<int>(); "
        "  c.dv := 99; "
        "  return c.ReadDV(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 99);
}

// ===========================================================================
// T6 — METHOD WITH A PARAMETER (also reads the inherited bare field).
// The pre-fix abort fires the same way regardless of whether the method
// has parameters. This case pins the value through a method that has
// BOTH a parameter and the inherited bare-id read — confirms the relaxed
// seed does not disturb parameter binding. Expected:
// `6 (da) + 41 (ba) + 3 (cell.tag) + 4 (bv.Length) + 2 ("hi".Length) = 56`.
// ===========================================================================
BEZEL_TEST(inherited_generic_base_field_bare_id_2909,
           method_with_parameter_spec_12_23_1) {
    auto r = runAotChecked(
        "module H; "
        "class Cell public tag: int := 3; end Cell; "
        "class Base<T> "
        "  public bv: T; "
        "  public owned cell: Cell := new Cell(); "
        "  public ba: int := 41; "
        "end Base; "
        "class Coll<U> : Base<string> "
        "  public da: int := 6; "
        "  public method ReadWithParam(p: string): int "
        "    return da + ba + cell.tag + bv.Length + p.Length; "
        "  end ReadWithParam; "
        "end Coll; "
        "class C public static method Main(): int "
        "  using var c := new Coll<int>(); "
        "  c.bv := \"abcd\"; "
        "  return c.ReadWithParam(\"hi\"); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 56);
}

// ===========================================================================
// T7 — A METHOD THAT CALLS ANOTHER METHOD (which also reads the inherited
// bare field). The pre-fix abort fires the same way regardless of whether
// the bare-id read is direct or wrapped in another method call. Pins the
// value through `Main -> Outer -> Inner -> bv.Length`. The OUTER method
// takes a U parameter so sema's overload-resolution also exercises —
// guards against a regression where the loosened seed accidentally affects
// a parameter's type lookup. Expected: 6 + 41 + 3 + 4 = 54.
// ===========================================================================
BEZEL_TEST(inherited_generic_base_field_bare_id_2909,
           method_calls_method_spec_12_23_1) {
    auto r = runAotChecked(
        "module H; "
        "class Cell public tag: int := 3; end Cell; "
        "class Base<T> "
        "  public bv: T; "
        "  public owned cell: Cell := new Cell(); "
        "  public ba: int := 41; "
        "end Base; "
        "class Coll<U> : Base<string> "
        "  public da: int := 6; "
        "  public method Inner(): int "
        "    return da + ba + cell.tag + bv.Length; "
        "  end Inner; "
        "  public method Outer(): int "
        "    return Inner(); "
        "  end Outer; "
        "end Coll; "
        "class C public static method Main(): int "
        "  using var c := new Coll<int>(); "
        "  c.bv := \"abcd\"; "
        "  return c.Outer(); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 54);
}

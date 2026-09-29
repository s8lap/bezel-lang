// Spec-derived tests for fix #4101 — §12.5 "Abstract classes and members" /
// §23.4 "Indexers" (v0.295).
//
// A non-abstract indexer with NO body at all — no accessors, no expression
// body, and no `abstract` modifier (`public property [i: int]: int;`) —
// compiled clean before this fix and reached codegen, where
// `ensureIndexerGetterFn` correctly declines to synthesize a getter for it
// (it has no Auto branch at all) and the read silently degraded to a
// zero-initialized value through #2434's degrade-disposition fallback — an
// "internal:" stderr note at runtime instead of a compile-time rejection.
// §23.4: "Indexers ... participate in virtual/override/sealed/abstract like
// methods" — a non-abstract indexer never gets automatic backing storage the
// way a plain named auto-property does (§23.2), so its Auto-bodied shape is
// legal ONLY as the §12.5 abstract shorthand. This fix extends
// `Resolver::checkTypeMethodBodies` (TypeResolution_Checks.cpp) — already
// enforcing the method analogue of this rule — with an indexer arm.
//
// Found sweeping #4026's accept/reject grid, filed as #4101 (branch
// lane-pool-5, R17 cycle-1 wave-1). Related but distinct: #4100 (abstract
// indexer shorthand WITH an initializer is dead code) is a separate,
// still-open defect — this fix does not touch that shape (verified below,
// `abstract_indexer_shorthand_with_initializer_still_open_4100`).

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

int runProg(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

}  // namespace

// =====================================================================
// REJECT — the issue's own two measured repros.
// =====================================================================

// REJECT — the bare declaration (issue's first repro): compiles clean today,
// with no invocation at all — the declaration alone must be rejected.
BEZEL_TEST(sema_indexer_body_presence_4101,
           bare_declaration_rejected_spec_12_5_23_4) {
    auto r = runAot(
        "module M; "
        "public class BaseIdxNB4101 "
        "  public property [i: int]: int; "
        "end BaseIdxNB4101; "
        "class ProgramIdxNB4101 "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end ProgramIdxNB4101;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagContains(r, "non-abstract indexer must have a body"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§12.5"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§23.4"));
}

// REJECT — the issue's second repro: the indexer is actually INVOKED. Before
// this fix this compiled (exit 0) and ran to a degraded zero value at
// runtime; now it must be a compile-time rejection instead, with the SAME
// message — pairing `failedAtCompileTimeWithRealLocation` with the message
// text per LANE_RULES §18 (the pre-fix "success" here was a real compiler
// exit 0, not a crash, but the discipline of pairing the helper with a
// message assertion is the same: a bare "did not compile" check would not
// distinguish this rejection from some unrelated one).
BEZEL_TEST(sema_indexer_body_presence_4101,
           invoked_bodyless_indexer_rejected_spec_12_5_23_4) {
    auto r = runAot(
        "module M; "
        "public class BaseIdxNB2_4101 "
        "  public property [i: int]: int; "
        "end BaseIdxNB2_4101; "
        "class ProgramIdxNB2_4101 "
        "  public static method Main(): int "
        "    var b := new BaseIdxNB2_4101(); "
        "    var v := b[0]; "
        "    return v; "
        "  end Main; "
        "end ProgramIdxNB2_4101;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagContains(r, "non-abstract indexer must have a body"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§12.5"));
}

// REJECT — a `static` context is irrelevant to this rule (indexers cannot be
// static at all, §23.5 — this row exists to confirm the rejection fires on
// the ordinary instance shape, matching the issue's own repro exactly, not
// gated on a static/instance distinction this file does not test elsewhere).
// (No separate row needed: covered by the two rows above.)

// =====================================================================
// ACCEPT — legitimate indexer (and named-property analogue) shapes must keep
// compiling (LANE_RULES §15's accept-side sweep — a false-reject would be
// worse than the bug this fixes).
// =====================================================================

// ACCEPT — an ordinary non-abstract indexer WITH a real block body (get +
// set): the common case, must be entirely unaffected.
BEZEL_TEST(sema_indexer_body_presence_4101,
           indexer_with_block_body_accepted_control_spec_23_4) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public class C1_4101 "
        "  private _v: int := 0; "
        "  public property [i: int]: int "
        "    get => _v; "
        "    set => do _v := value; end; "
        "  end; "
        "end C1_4101; "
        "class Program1_4101 "
        "  public static method Main(): int "
        "    var c := new C1_4101(); "
        "    c[0] := 5; "
        "    return c[0]; "
        "  end Main; "
        "end Program1_4101;"),
        5);
}

// ACCEPT — an expression-bodied (get-only) indexer: a real body of a
// different PropertyBodyKind (Expression, not Auto) — must be unaffected.
BEZEL_TEST(sema_indexer_body_presence_4101,
           expression_bodied_indexer_accepted_control_spec_23_4) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public class C5_4101 "
        "  public property [i: int]: int => i * 2; "
        "end C5_4101; "
        "class Program5_4101 "
        "  public static method Main(): int "
        "    var c := new C5_4101(); "
        "    return c[3]; "
        "  end Main; "
        "end Program5_4101;"),
        6);
}

// ACCEPT — the §12.5 abstract shorthand on an indexer (`abstract property
// [i]: T;`, no body at all, but WITH the `abstract` modifier) plus a
// concrete get-only override: the exact shape this rule must continue to
// admit — an abstract indexer's Auto/no-body form is the intended legal use
// of that shape, not the defect.
BEZEL_TEST(sema_indexer_body_presence_4101,
           abstract_indexer_shorthand_with_override_accepted_control_spec_12_5) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public abstract class Base2_4101 "
        "  public abstract property [i: int]: int; "
        "end Base2_4101; "
        "class Derived2_4101 : Base2_4101 "
        "  private _v: int := 7; "
        "  public override property [i: int]: int => _v; "
        "end Derived2_4101; "
        "class Program2_4101 "
        "  public static method Main(): int "
        "    var d: Base2_4101 := new Derived2_4101(); "
        "    return d[0]; "
        "  end Main; "
        "end Program2_4101;"),
        7);
}

// ACCEPT — an interface indexer signature (§23.4: "Indexers may be declared
// in interfaces with the body-less form"): Auto-bodied, no `abstract`
// modifier at all (interface members are implicitly abstract via the
// interface, not the modifier) — must stay accepted exactly like the
// interface-property analogue in #4026's test file.
BEZEL_TEST(sema_indexer_body_presence_4101,
           interface_indexer_signature_accepted_control_spec_23_4) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public interface IIdx4101 "
        "  property [i: int]: int; "
        "end IIdx4101; "
        "public class Impl3_4101 : IIdx4101 "
        "  private _v: int := 3; "
        "  public property [i: int]: int "
        "    get => _v; "
        "    set => do _v := value; end; "
        "  end; "
        "end Impl3_4101; "
        "class Program3_4101 "
        "  public static method Main(): int "
        "    var x: IIdx4101 := new Impl3_4101(); "
        "    return x[0]; "
        "  end Main; "
        "end Program3_4101;"),
        3);
}

// ACCEPT — a `virtual` base indexer with a real body, overridden with a real
// body (`override`, no `abstract` anywhere): confirms the rule is not
// accidentally gated on the presence of `override`/`virtual` alone — only
// the Auto/no-body SHAPE matters.
BEZEL_TEST(sema_indexer_body_presence_4101,
           virtual_override_indexer_with_bodies_accepted_control_spec_12_1) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public class Base4_4101 "
        "  private _v: int := 1; "
        "  public virtual property [i: int]: int "
        "    get => _v; "
        "    set => do _v := value; end; "
        "  end; "
        "end Base4_4101; "
        "class Derived4_4101 : Base4_4101 "
        "  private _w: int := 9; "
        "  public override property [i: int]: int "
        "    get => _w; "
        "    set => do _w := value; end; "
        "  end; "
        "end Derived4_4101; "
        "class Program4_4101 "
        "  public static method Main(): int "
        "    var d: Base4_4101 := new Derived4_4101(); "
        "    return d[0]; "
        "  end Main; "
        "end Program4_4101;"),
        9);
}

// ACCEPT — a generic class's indexer: the rule reads only `bodyKind` and the
// `abstract` modifier off the AST, so it cannot be sensitive to the
// enclosing type's generic parameters.
BEZEL_TEST(sema_indexer_body_presence_4101,
           generic_type_indexer_accepted_control_spec_23_4) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public class Box6_4101<T> "
        "  private _v: T; "
        "  public constructor(v: T) "
        "    _v := v; "
        "  end; "
        "  public property [i: int]: T => _v; "
        "end Box6_4101; "
        "class Program6_4101 "
        "  public static method Main(): int "
        "    var b := new Box6_4101<int>(42); "
        "    return b[0]; "
        "  end Main; "
        "end Program6_4101;"),
        42);
}

// ACCEPT (named-property analogue) — a plain non-abstract Auto-bodied NAMED
// property with NO initializer (`public property Value: int;`): unlike an
// indexer, this IS a legitimate normal auto-property with real compiler-
// synthesized backing storage (§23.2) — the check must be scoped to
// `ps->isIndexer` and must not reject this.
BEZEL_TEST(sema_indexer_body_presence_4101,
           named_autoprop_no_initializer_accepted_control_spec_23_2) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public class C10_4101 "
        "  public property Value: int; "
        "end C10_4101; "
        "class Program10_4101 "
        "  public static method Main(): int "
        "    var c := new C10_4101(); "
        "    c.Value := 8; "
        "    return c.Value; "
        "  end Main; "
        "end Program10_4101;"),
        8);
}

// ACCEPT (named-property analogue) — a plain non-abstract Auto-bodied NAMED
// property WITH an initializer: the ordinary auto-property idiom, must be
// unaffected.
BEZEL_TEST(sema_indexer_body_presence_4101,
           named_autoprop_with_initializer_accepted_control_spec_23_2) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public class C8_4101 "
        "  public property Value: int := 5; "
        "end C8_4101; "
        "class Program8_4101 "
        "  public static method Main(): int "
        "    var c := new C8_4101(); "
        "    return c.Value; "
        "  end Main; "
        "end Program8_4101;"),
        5);
}

// ACCEPT (named-property analogue) — an abstract NAMED property shorthand
// with a concrete override: the pre-existing #4026-adjacent shape, confirms
// this new indexer arm did not disturb the sibling named-property path
// (which is handled by a different check, `checkAbstractPropertyInitializers`,
// and by this same function's untouched non-indexer branch — a `PropertySymbol`
// that is not an indexer is skipped by this fix's `!ps->isIndexer` guard
// before `bodyKind`/`abstract` are even read).
BEZEL_TEST(sema_indexer_body_presence_4101,
           abstract_named_property_with_override_accepted_control_spec_12_5) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public abstract class Base9_4101 "
        "  public abstract property Name: int; "
        "end Base9_4101; "
        "class Derived9_4101 : Base9_4101 "
        "  public override property Name: int => 11; "
        "end Derived9_4101; "
        "class Program9_4101 "
        "  public static method Main(): int "
        "    var d: Base9_4101 := new Derived9_4101(); "
        "    return d.Name; "
        "  end Main; "
        "end Program9_4101;"),
        11);
}

// REJECT — #4100 (abstract indexer shorthand WITH an initializer is dead
// code per §12.5: an abstract member has no backing storage for the
// initializer to write into, so it could never run). Previously accepted
// due to the `p.isIndexer` guard in `isAbstractPropertyShorthand` (#4037
// fix removes that guard so the shared predicate covers indexers correctly).
BEZEL_TEST(sema_indexer_body_presence_4101,
           abstract_indexer_shorthand_with_initializer_rejected_4100) {
    auto r = runAot(
        "module M; "
        "public abstract class BaseIdxInit4100 "
        "  public abstract property [i: int]: int := 5; "
        "end BaseIdxInit4100; "
        "class ProgramIdxInit4100 "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end ProgramIdxInit4100;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "may not declare an initializer"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§12.5"));
}

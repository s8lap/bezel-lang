// Spec-derived tests for fix #4110 — §12.5 "Abstract classes and members" /
// §23.4 "Indexers" (v0.298).
//
// An `abstract` member has no backing storage and no reachable call site of
// its own: dispatch through a base-typed reference always resolves to the
// most-derived override (§12.1), so a body an abstract member declared
// could never run. Two existing fixes already reject the two AUTO-bodied
// dead-storage shapes:
//   - #4026 rejects `abstract property Name: T := expr;` (the Auto
//     shorthand carrying an initializer) — guarded by the shared
//     `isAbstractPropertyShorthand` predicate (sema/Symbol.hpp), which
//     requires `PropertyBodyKind::Auto` AND excludes indexers by design.
//   - #4101 rejects a non-abstract indexer with no body at all (the
//     Auto-bodied indexer, legal only when abstract).
//
// Neither touches an EXPLICIT `=>` expression body or a Block body with a
// real (non-stub) accessor implementation on an `abstract` member — and
// this shape compiled clean before this fix, for BOTH named properties and
// indexers (measured):
//
//   abstract property [i: int]: int => 5;   // indexer, expression body
//   abstract property Name: int => 5;       // named property, expression body
//
// Dispatch always reaches the override (measured: an indexer repro
// identical in shape to the above exits 9 through a concrete override, not
// through the abstract member's own body), so the body is dead code the
// compiler accepted without a word.
//
// This fix adds `Resolver::checkAbstractPropertyBodies`
// (TypeResolution_Checks.cpp), called alongside #4026's
// `checkAbstractPropertyInitializers` (TypeResolution.cpp). It cannot reuse
// `isAbstractPropertyShorthand`: that predicate is deliberately narrowed to
// `PropertyBodyKind::Auto` (an Expression/Block body has no autoInitializer
// slot to guard) and deliberately excludes indexers (narrow #4037 measured
// that a naive substitution for the indexer domain silently accepts an
// illegally-added indexer setter) — so this reads the same raw AST facts
// (`bodyKind`, the `abstract` modifier, and each accessor's own
// `PropertyAccessorBodyKind`) directly, for both member kinds.
//
// Accept-side care (LANE_RULES §15): the Block form is ALSO the legitimate
// way to spell an abstract member needing more than the Auto shorthand's
// implicit get-only accessor — `abstract property Val: string get; set;
// end;`, #4026's own accepted control — where every accessor is itself
// `PropertyAccessorBodyKind::Abstract` (the bare `;` stub). Only an
// accessor carrying REAL code is this fix's target; the stub form must not
// regress (pinned below).

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
// REJECT — the issue's own two measured repros, plus their Block-body
// analogues.
// =====================================================================

// REJECT — abstract INDEXER, expression body (the issue's own repro).
BEZEL_TEST(sema_abstract_property_indexer_dead_body_4110,
           indexer_expression_body_rejected_spec_12_5_23_4) {
    auto r = runAot(
        "module M; "
        "public abstract class BaseIdxExpr4110 "
        "  public abstract property [i: int]: int => 5; "
        "end BaseIdxExpr4110; "
        "class ProgramIdxExpr4110 "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end ProgramIdxExpr4110;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "abstract indexer may not have a body"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§12.5"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§23.4"));
}

// REJECT — abstract NAMED property, expression body (the issue's own repro).
BEZEL_TEST(sema_abstract_property_indexer_dead_body_4110,
           named_property_expression_body_rejected_spec_12_5) {
    auto r = runAot(
        "module M; "
        "public abstract class BaseNamedExpr4110 "
        "  public abstract property Name4110: int => 5; "
        "end BaseNamedExpr4110; "
        "class ProgramNamedExpr4110 "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end ProgramNamedExpr4110;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "abstract property `Name4110` may not have a body"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§12.5"));
}

// REJECT — abstract INDEXER, Block body with a REAL (non-stub) getter — the
// distinct-body-kind case: bodyKind is Block, not Expression, so this row
// exercises the accessor-scan branch rather than the top-level-Expression
// branch above.
BEZEL_TEST(sema_abstract_property_indexer_dead_body_4110,
           indexer_block_body_real_accessor_rejected_spec_12_5_23_4) {
    auto r = runAot(
        "module M; "
        "public abstract class BaseIdxBlock4110 "
        "  public abstract property [i: int]: int "
        "    get => i; "
        "  end; "
        "end BaseIdxBlock4110; "
        "class ProgramIdxBlock4110 "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end ProgramIdxBlock4110;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "abstract indexer may not have a body"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§12.5"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§23.4"));
}

// REJECT — abstract NAMED property, Block body with a REAL (non-stub)
// getter.
BEZEL_TEST(sema_abstract_property_indexer_dead_body_4110,
           named_property_block_body_real_accessor_rejected_spec_12_5) {
    auto r = runAot(
        "module M; "
        "public abstract class BaseNamedBlock4110 "
        "  public abstract property Name4110b: int "
        "    get => 5; "
        "  end; "
        "end BaseNamedBlock4110; "
        "class ProgramNamedBlock4110 "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end ProgramNamedBlock4110;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(
        r, "abstract property `Name4110b` may not have a body"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§12.5"));
}

// =====================================================================
// ACCEPT — legitimate abstract/virtual/override/interface shapes must keep
// compiling (LANE_RULES §15).
// =====================================================================

// ACCEPT — abstract NAMED property with NO body at all (the Auto shorthand,
// #4026's own domain) must be unaffected.
BEZEL_TEST(sema_abstract_property_indexer_dead_body_4110,
           named_property_no_body_still_accepted_control_spec_12_5) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public abstract class BaseNamedNB4110 "
        "  public abstract property Name4110c: int; "
        "end BaseNamedNB4110; "
        "class DNamedNB4110 : BaseNamedNB4110 "
        "  public override property Name4110c: int => 11; "
        "end DNamedNB4110; "
        "class ProgramNamedNB4110 "
        "  public static method Main(): int "
        "    var d := new DNamedNB4110(); "
        "    return d.Name4110c; "
        "  end Main; "
        "end ProgramNamedNB4110;"),
        11);
}

// ACCEPT — abstract INDEXER with NO body at all (#4101's own domain) must be
// unaffected.
BEZEL_TEST(sema_abstract_property_indexer_dead_body_4110,
           indexer_no_body_still_accepted_control_spec_12_5_23_4) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public abstract class BaseIdxNB4110 "
        "  public abstract property [i: int]: int; "
        "end BaseIdxNB4110; "
        "class DIdxNB4110 : BaseIdxNB4110 "
        "  public override property [i: int]: int => i * 3; "
        "end DIdxNB4110; "
        "class ProgramIdxNB4110 "
        "  public static method Main(): int "
        "    var d := new DIdxNB4110(); "
        "    return d[2]; "
        "  end Main; "
        "end ProgramIdxNB4110;"),
        6);
}

// ACCEPT — abstract NAMED property, Block form with BOTH accessors given
// explicit `;` (abstract stub) bodies — this fix's own load-bearing
// anti-regression, mirroring #4026's
// `abstract_block_form_accessors_accepted_control_spec_12_5`. Every
// accessor here is `PropertyAccessorBodyKind::Abstract`, not a real body —
// must not be caught by the new accessor scan.
BEZEL_TEST(sema_abstract_property_indexer_dead_body_4110,
           named_property_block_form_stub_accessors_still_accepted_control) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public abstract class BaseNamedStub4110 "
        "  public abstract property Val4110: string "
        "    get; "
        "    set; "
        "  end; "
        "end BaseNamedStub4110; "
        "class DNamedStub4110 : BaseNamedStub4110 "
        "  private _v4110: string := \"blk\"; "
        "  public override property Val4110: string "
        "    get => _v4110; "
        "    set => _v4110 := value; "
        "  end; "
        "end DNamedStub4110; "
        "class ProgramNamedStub4110 "
        "  public static method Main(): int "
        "    var d := new DNamedStub4110(); "
        "    return d.Val4110.Length; "
        "  end Main; "
        "end ProgramNamedStub4110;"),
        3);
}

// ACCEPT — a concrete `override` supplying a real body must keep compiling
// and dispatching correctly (indexer form — covered above implicitly by the
// two "no body" accept rows' overrides, which already carry real
// `Expression` bodies and pass; this row isolates it as its own control on
// the OVERRIDE member itself, not the abstract base).
BEZEL_TEST(sema_abstract_property_indexer_dead_body_4110,
           concrete_override_with_body_still_accepted_control) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public abstract class BaseOverride4110 "
        "  public abstract property [i: int]: int; "
        "end BaseOverride4110; "
        "class DOverride4110 : BaseOverride4110 "
        "  public override property [i: int]: int "
        "    get => i + 100; "
        "  end; "
        "end DOverride4110; "
        "class ProgramOverride4110 "
        "  public static method Main(): int "
        "    var d := new DOverride4110(); "
        "    return d[1]; "
        "  end Main; "
        "end ProgramOverride4110;"),
        101);
}

// ACCEPT — `virtual` (non-abstract) with a body must keep compiling — a
// virtual member's own body IS reachable (it runs unless overridden), so
// this rule must never fire on `virtual`.
BEZEL_TEST(sema_abstract_property_indexer_dead_body_4110,
           virtual_named_property_with_body_still_accepted_control) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public class BaseVirtual4110 "
        "  public virtual property Name4110d: int => 42; "
        "end BaseVirtual4110; "
        "class ProgramVirtual4110 "
        "  public static method Main(): int "
        "    var b := new BaseVirtual4110(); "
        "    return b.Name4110d; "
        "  end Main; "
        "end ProgramVirtual4110;"),
        42);
}

// ACCEPT — `virtual` (non-abstract) INDEXER with a body must keep
// compiling, mirroring the named-property control above.
BEZEL_TEST(sema_abstract_property_indexer_dead_body_4110,
           virtual_indexer_with_body_still_accepted_control) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public class BaseVirtualIdx4110 "
        "  public virtual property [i: int]: int => i + 1; "
        "end BaseVirtualIdx4110; "
        "class ProgramVirtualIdx4110 "
        "  public static method Main(): int "
        "    var b := new BaseVirtualIdx4110(); "
        "    return b[4]; "
        "  end Main; "
        "end ProgramVirtualIdx4110;"),
        5);
}

// ACCEPT — an interface property signature (implicitly abstract via the
// interface, no `abstract` modifier) is unaffected — this check's guard
// requires the literal `abstract` modifier, which an interface member never
// carries.
BEZEL_TEST(sema_abstract_property_indexer_dead_body_4110,
           interface_property_signature_still_accepted_control) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public interface IThing4110 "
        "  readonly property Name4110e: int; "
        "end IThing4110; "
        "public class Impl4110 : IThing4110 "
        "  public property Name4110e: int => 8; "
        "end Impl4110; "
        "class ProgramIface4110 "
        "  public static method Main(): int "
        "    var t: IThing4110 := new Impl4110(); "
        "    return t.Name4110e; "
        "  end Main; "
        "end ProgramIface4110;"),
        8);
}

// ACCEPT — an interface INDEXER signature is likewise unaffected.
BEZEL_TEST(sema_abstract_property_indexer_dead_body_4110,
           interface_indexer_signature_still_accepted_control) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public interface IThingIdx4110 "
        "  readonly property [i: int]: int; "
        "end IThingIdx4110; "
        "public class ImplIdx4110 : IThingIdx4110 "
        "  public property [i: int]: int => i + 9; "
        "end ImplIdx4110; "
        "class ProgramIfaceIdx4110 "
        "  public static method Main(): int "
        "    var t: IThingIdx4110 := new ImplIdx4110(); "
        "    return t[1]; "
        "  end Main; "
        "end ProgramIfaceIdx4110;"),
        10);
}

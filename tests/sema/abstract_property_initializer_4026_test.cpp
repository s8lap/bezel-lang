// Spec-derived tests for fix #4026 — §12.5 "Abstract classes and members"
// (v0.295).
//
// `abstract property Name: T := expr;` (a class-level abstract member with
// an initializer) parsed and compiled clean before this fix, but the
// initializer could never execute: an abstract Auto-shorthand property has
// no backing storage (§12.5: "`abstract` on a member declares it without a
// body"; `sema::isAbstractPropertyShorthand`, sema/Symbol.hpp, is already
// excluded from every storage-generating pass —
// `hasAstAutoPropertyDecl`/`isAutoPropertyStorageSymbol`,
// SymbolCollection.cpp). `expr` was parsed, type-checked, and then never
// referenced by anything downstream — dead code with no diagnostic, not even
// a warning. This fix adds a dedicated rejection
// (`Resolver::checkAbstractPropertyInitializers`, TypeResolution_Checks.cpp)
// naming §12.5.
//
// Found auditing #4018/#4020's own fix (the shared
// `isAbstractPropertyShorthand` predicate); filed and fixed separately per
// CLAUDE.md's decline-with-reasons discipline (out of scope for #4018/#4020
// itself).

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
// REJECT — the issue's own measured repro.
// =====================================================================

BEZEL_TEST(sema_abstract_property_initializer_4026,
           abstract_property_with_initializer_rejected_spec_12_5) {
    auto r = runAot(
        "module M; "
        "public abstract class BaseAI4026 "
        "  public abstract property Name: string := \"default\"; "
        "end BaseAI4026; "
        "class ProgramAI4026 "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end ProgramAI4026;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagContains(r, "abstract property `Name` may not declare an "
                        "initializer"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§12.5"));
}

// REJECT — a non-`string`/primitive initializer (a `new` expression) hits
// the identical check; confirms the rejection isn't accidentally gated on a
// foldable-constant initializer path (several unrelated checks in this file
// take a "constant vs. general expression" fork the way #2926's §5.1 literal
// check does).
BEZEL_TEST(sema_abstract_property_initializer_4026,
           abstract_property_with_new_initializer_rejected_spec_12_5) {
    auto r = runAot(
        "module M; "
        "class Widget4026 end Widget4026; "
        "public abstract class BaseAINew4026 "
        "  public abstract property Current: Widget4026 := new Widget4026(); "
        "end BaseAINew4026; "
        "class ProgramAINew4026 "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end ProgramAINew4026;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagContains(r, "abstract property `Current` may not declare an "
                        "initializer"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§12.5"));
}

// REJECT — `readonly abstract property` + initializer: the `readonly`
// modifier does not exempt the shorthand from this rule (both are Auto-
// bodied; `isAbstractPropertyShorthand` does not look at `readonly` at all).
BEZEL_TEST(sema_abstract_property_initializer_4026,
           abstract_readonly_property_with_initializer_rejected_spec_12_5) {
    auto r = runAot(
        "module M; "
        "public abstract class BaseAIRO4026 "
        "  public abstract readonly property Name: string := \"default\"; "
        "end BaseAIRO4026; "
        "class ProgramAIRO4026 "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end ProgramAIRO4026;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagContains(r, "abstract property `Name` may not declare an "
                        "initializer"));
    BEZEL_EXPECT_TRUE(diagContains(r, "§12.5"));
}

// =====================================================================
// ACCEPT — legitimate abstract-property and auto-property shapes must keep
// compiling (LANE_RULES §15's accept-side sweep — a false-reject would be
// worse than the bug this fixes).
// =====================================================================

// ACCEPT — the ordinary abstract shorthand, no initializer at all (the
// correct §12.5 form; must be entirely unaffected).
BEZEL_TEST(sema_abstract_property_initializer_4026,
           abstract_property_without_initializer_accepted_control_spec_12_5) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public abstract class BaseNoInit4026 "
        "  public abstract property Name: string; "
        "end BaseNoInit4026; "
        "class DNoInit4026 : BaseNoInit4026 "
        "  public override property Name: string => \"impl\"; "
        "end DNoInit4026; "
        "class C "
        "  public static method Main(): int "
        "    var d := new DNoInit4026(); "
        "    return d.Name.Length; "
        "  end Main; "
        "end C;"),
        4);
}

// ACCEPT — an ORDINARY (non-abstract) auto-property with an initializer:
// this is the entire point of the auto-property form and must be completely
// unaffected by a check scoped to the abstract shorthand.
BEZEL_TEST(sema_abstract_property_initializer_4026,
           ordinary_autoprop_with_initializer_accepted_control_spec_23_1) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public class HolderOrd4026 "
        "  public property Val: string := \"hello\"; "
        "end HolderOrd4026; "
        "class C "
        "  public static method Main(): int "
        "    var h := new HolderOrd4026(); "
        "    return h.Val.Length; "
        "  end Main; "
        "end C;"),
        5);
}

// ACCEPT — a CONCRETE override of an abstract property, itself carrying an
// initializer: the override is a normal (non-abstract) auto-property, which
// DOES get real backing storage — must not be caught by the base's
// abstract-shorthand rule.
BEZEL_TEST(sema_abstract_property_initializer_4026,
           override_with_initializer_accepted_control_spec_12_5) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public abstract class BaseOverrideInit4026 "
        "  public abstract property Val: string; "
        "end BaseOverrideInit4026; "
        "class DOverrideInit4026 : BaseOverrideInit4026 "
        "  public override readonly property Val: string := \"concrete\"; "
        "end DOverrideInit4026; "
        "class C "
        "  public static method Main(): int "
        "    var d := new DOverrideInit4026(); "
        "    return d.Val.Length; "
        "  end Main; "
        "end C;"),
        8);
}

// ACCEPT — a `static readonly` auto-property with an initializer: unrelated
// to `abstract` entirely; must be unaffected.
BEZEL_TEST(sema_abstract_property_initializer_4026,
           static_readonly_autoprop_with_initializer_accepted_control) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public class HolderStatic4026 "
        "  public static readonly property Val: string := \"static\"; "
        "end HolderStatic4026; "
        "class C "
        "  public static method Main(): int "
        "    return HolderStatic4026.Val.Length; "
        "  end Main; "
        "end C;"),
        6);
}

// ACCEPT — an abstract property with BOTH accessors given explicit `;`
// (abstract) bodies, Block-bodied rather than Auto — the grammar has no
// initializer slot in this form at all, so it can never trip this check
// (distinct code path: `isAbstractPropertyShorthand` requires
// `bodyKind == Auto`).
BEZEL_TEST(sema_abstract_property_initializer_4026,
           abstract_block_form_accessors_accepted_control_spec_12_5) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public abstract class BaseBlock4026 "
        "  public abstract property Val: string "
        "    get; "
        "    set; "
        "  end; "
        "end BaseBlock4026; "
        "class DBlock4026 : BaseBlock4026 "
        "  private _v: string := \"blk\"; "
        "  public override property Val: string "
        "    get => _v; "
        "    set => _v := value; "
        "  end; "
        "end DBlock4026; "
        "class C "
        "  public static method Main(): int "
        "    var d := new DBlock4026(); "
        "    return d.Val.Length; "
        "  end Main; "
        "end C;"),
        3);
}

// ACCEPT — an interface property (signature form, `PropertyBodyKind::Auto`
// but no `abstract` keyword — interface members are implicitly abstract via
// the interface, not the modifier) is unaffected: `isAbstractPropertyShorthand`
// requires the literal `abstract` modifier, which an interface member never
// carries.
BEZEL_TEST(sema_abstract_property_initializer_4026,
           interface_property_signature_accepted_control_spec_11_2) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public interface IHolder4026 "
        "  property Val: string; "
        "end IHolder4026; "
        "public class HolderIface4026 : IHolder4026 "
        "  public property Val: string := \"iface\"; "
        "end HolderIface4026; "
        "class C "
        "  public static method Main(): int "
        "    var h: IHolder4026 := new HolderIface4026(); "
        "    return h.Val.Length; "
        "  end Main; "
        "end C;"),
        5);
}

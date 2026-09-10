// Spec-derived tests for fix #4170 — §35.1 "Declaration", §35.2 "Members",
// §35.3 "Construction and payload ownership" (choice half); §5.4/§10 (enum
// half); §17.7 "The `new` operator" (the target-typed `new()` shorthand
// itself) (v0.295).
//
// Fix #4163 (tests/sema/new_choice_construction_rejected_4163_test.cpp)
// closed `new` on a choice for the EXPLICIT `new ChoiceType(args)` spelling
// only — the NewExpr-typing arm in ExprTypecheck_Exprs.cpp, gated on
// `ne->type` being present. It did NOT close the same construct reached via
// the §17.7 target-typed `new()` SHORTHAND, which takes its construction
// target from the declared type at two separate call sites in
// ExprTypecheck.cpp (a shorthand NewExpr's own `->type` is null by
// definition):
//   - field-initializer shorthand (~line 12173, pre-fix)
//   - local-variable-declaration shorthand (~line 13293, pre-fix)
// Both sites set the shorthand NewExpr's `exprType` to the declared target
// type unconditionally, then called `checkNewExprArgs` ONLY when the
// target's `TypeDeclKind` was `Class` — no Enum or Choice arm at all. A
// shorthand `new()` target-typed to a Choice reached codegen with no case
// identity, exactly as the (#4163-fixed) explicit spelling once did, and
// crashed the compiler the same way.
//
// MEASURED (issue #4170's own repros, verbatim, pre-fix):
//   `var x: ChoiceSHC := new();` followed by a `case` statement over `x` —
//   direct `bezelc` invocation, "Segmentation fault (core dumped)", exit
//   139 (signal 11) — the SAME crash class #4163 fixed for the explicit
//   spelling, reached through the shorthand path #4163 did not touch.
//   `var x: ColorSHC := new();` (enum) followed by a `case` statement over
//   `x` — a DIFFERENT, non-fatal symptom: codegen produces malformed IR
//   ("Both operands to ICmp instruction are not of the same type!") and
//   dies at the IR-verifier backstop, exit 1, with no real source location
//   in the diagnostic text.
//
// FIX: factor the enum/choice rejection out of the explicit-`new` arm into
// one shared predicate, `ExprTC::rejectNewOnEnumOrChoice` (declared in
// ExprTypecheck_impl.hpp, defined in ExprTypecheck_Exprs.cpp beside the
// closed-constructor-surface check it neighbors), and call it from BOTH
// §17.7 shorthand call sites in ExprTypecheck.cpp as well as the explicit
// site — a single authority instead of a third and fourth spelling of the
// same two checks (CLAUDE.md / LANE_RULES.md §6).
//
// Because the pre-fix CHOICE symptom is a compiler SIGSEGV, those rows use
// `failedAtCompileTimeWithRealLocation` (aot_result_asserts.hpp) PAIRED
// WITH message-text (LANE_RULES §18) exactly as #4163's own crash row does:
// `runAot`'s crash path reports "runAot: compiler CRASHED (signal N) …" at
// SourceLocation{}, which the helper's own exclusion list already screens
// out, so a choice row reads FALSE pre-fix (crash, not a real diagnostic)
// and TRUE post-fix (real `<path>:<line>:<col>:` diagnostic with the
// §35.1/§35.2/§35.3 text).
//
// The pre-fix ENUM symptom is different — an IR-verifier failure, not a
// raw signal — but is EQUALLY excluded from `failedAtCompileTimeWithRealLocation`:
// the verifier's own diagnostic ("bezelc: codegen error: IR verification
// failed …") carries no `<line>:<col>:` prefix at all (verified: the raw
// `bezelc` output above has none), so the helper still reads FALSE pre-fix
// for the right reason and TRUE post-fix once the real §5.4/§10 diagnostic
// (with a real location) fires instead. Message-text is asserted on every
// row regardless, per §18's lesson that the location helper alone is not
// sufficient when the pre-fix behavior routes through a backstop.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

int runProg(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// =====================================================================
// REJECT: the issue's own repros, verbatim shape — local-decl shorthand
// followed by a `case` statement over the result.
// =====================================================================

BEZEL_TEST(sema_new_shorthand_enum_choice_construction_rejected_4170,
           new_choice_local_decl_shorthand_then_case_no_longer_crashes_rejected_spec_35_1_17_7) {
    auto r = runAot(
        "module M; "
        "public choice ChoiceSHC Has(H: int), Empty(); end ChoiceSHC; "
        "class ProgramSHC "
        "  public static method Main(): int "
        "    var x: ChoiceSHC := new(); "
        "    case x of "
        "      Has(var h) => Console.WriteLine(h.ToString()), "
        "      Empty()    => Console.WriteLine(\"Empty\") "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end ProgramSHC;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "`new` cannot construct a choice"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§35.1/§35.2/§35.3"));
}

BEZEL_TEST(sema_new_shorthand_enum_choice_construction_rejected_4170,
           new_enum_local_decl_shorthand_then_case_hits_ir_verifier_pre_fix_rejected_spec_5_4_10_17_7) {
    auto r = runAot(
        "module M; "
        "public enum ColorSHC: int Red, Green; end ColorSHC; "
        "class ProgramSHEC "
        "  public static method Main(): int "
        "    var x: ColorSHC := new(); "
        "    case x of "
        "      Red   => Console.WriteLine(\"red\"), "
        "      Green => Console.WriteLine(\"green\") "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end ProgramSHEC;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "`new` cannot construct an enum"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§5.4/§10"));
}

// =====================================================================
// REJECT: the field-initializer shorthand twin of each repro above —
// ExprTypecheck.cpp's OTHER shorthand call site (~line 12173 pre-fix),
// which had the identical missing-Enum/Choice-arm gap.
// =====================================================================

BEZEL_TEST(sema_new_shorthand_enum_choice_construction_rejected_4170,
           new_choice_field_initializer_shorthand_rejected_spec_35_1_17_7) {
    auto r = runAot(
        "module M; "
        "public choice ChoiceSHF Has(H: int), Empty(); end ChoiceSHF; "
        "class HolderSHF "
        "  public Slot: ChoiceSHF := new(); "
        "end HolderSHF; "
        "class ProgramSHF "
        "  public static method Main(): int "
        "    var h := new HolderSHF(); "
        "    return 0; "
        "  end Main; "
        "end ProgramSHF;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "`new` cannot construct a choice"));
}

BEZEL_TEST(sema_new_shorthand_enum_choice_construction_rejected_4170,
           new_enum_field_initializer_shorthand_rejected_spec_5_4_10_17_7) {
    auto r = runAot(
        "module M; "
        "public enum ColorSHF: int Red, Green; end ColorSHF; "
        "class HolderSHEF "
        "  public Slot: ColorSHF := new(); "
        "end HolderSHEF; "
        "class ProgramSHEF "
        "  public static method Main(): int "
        "    var h := new HolderSHEF(); "
        "    return 0; "
        "  end Main; "
        "end ProgramSHEF;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "`new` cannot construct an enum"));
}

// =====================================================================
// REJECT: shape sweep on the shorthand path — a type-alias target (mirrors
// #4163's own "a type-alias-to-choice is rejected through the alias too"
// verification for the explicit spelling) and a payload-shaped shorthand
// argument list (the shorthand form is `new(args)`, not only `new()` — the
// rejection must fire independent of argument shape, exactly as #4163's
// `new_choice_wrong_arity_rejected` pins for the explicit spelling).
// =====================================================================

BEZEL_TEST(sema_new_shorthand_enum_choice_construction_rejected_4170,
           new_choice_local_decl_shorthand_via_type_alias_rejected_spec_35_1_17_7) {
    auto r = runAot(
        "module M; "
        "public choice ChoiceSHA Has(H: int), Empty(); end ChoiceSHA; "
        "type ChoiceAliasSHA := ChoiceSHA; "
        "class ProgramSHA "
        "  public static method Main(): int "
        "    var x: ChoiceAliasSHA := new(); "
        "    return 0; "
        "  end Main; "
        "end ProgramSHA;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "`new` cannot construct a choice"));
}

BEZEL_TEST(sema_new_shorthand_enum_choice_construction_rejected_4170,
           new_enum_local_decl_shorthand_via_type_alias_rejected_spec_5_4_10_17_7) {
    auto r = runAot(
        "module M; "
        "public enum ColorSHA: int Red, Green; end ColorSHA; "
        "type ColorAliasSHA := ColorSHA; "
        "class ProgramSHEA "
        "  public static method Main(): int "
        "    var x: ColorAliasSHA := new(); "
        "    return 0; "
        "  end Main; "
        "end ProgramSHEA;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "`new` cannot construct an enum"));
}

BEZEL_TEST(sema_new_shorthand_enum_choice_construction_rejected_4170,
           new_choice_local_decl_shorthand_with_payload_args_rejected_spec_35_1_17_7) {
    auto r = runAot(
        "module M; "
        "public choice ChoiceSHP Has(H: int), Empty(); end ChoiceSHP; "
        "class ProgramSHP "
        "  public static method Main(): int "
        "    var x: ChoiceSHP := new(7); "
        "    return 0; "
        "  end Main; "
        "end ProgramSHP;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "`new` cannot construct a choice"));
}

// A GENERIC (stdlib) choice reached via the shorthand — `Result<T, E>`
// (stdlib §2.20) — gets the identical rejection, mirroring #4163's own
// `new_generic_stdlib_choice_rejected` control for the explicit spelling.
BEZEL_TEST(sema_new_shorthand_enum_choice_construction_rejected_4170,
           new_generic_stdlib_choice_local_decl_shorthand_rejected_spec_35_1_17_7) {
    auto r = runAot(
        "module M; "
        "class ProgramSHG "
        "  public static method Main(): int "
        "    var x: Result<int, string> := new(); "
        "    return 0; "
        "  end Main; "
        "end ProgramSHG;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "`new` cannot construct a choice"));
}

// =====================================================================
// ACCEPT-SIDE SWEEP (LANE_RULES §15): the legitimate shorthand `new()`
// uses this fix must NOT touch — a Class and a Record, in BOTH the
// local-decl and field-initializer positions, including a generic class
// and a nested type. (#4163's own file already sweeps the EXPLICIT `new
// T(...)` accept side for record/class/enum/interface controls; this file
// covers the SHORTHAND path specifically, since that is the path this fix
// touches.)
// =====================================================================

BEZEL_TEST(sema_new_shorthand_enum_choice_construction_rejected_4170,
           accept_class_local_decl_shorthand_compiles_and_runs_spec_17_7) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "class WidgetACC4170 "
        "  public property N: int; "
        "  public constructor() N := 7; end constructor; "
        "end WidgetACC4170; "
        "class ProgramACC4170 "
        "  public static method Main(): int "
        "    var w: WidgetACC4170 := new(); "
        "    return w.N; "
        "  end Main; "
        "end ProgramACC4170;"),
        7);
}

BEZEL_TEST(sema_new_shorthand_enum_choice_construction_rejected_4170,
           accept_class_field_initializer_shorthand_compiles_and_runs_spec_17_7) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "class WidgetACF4170 "
        "  public property N: int; "
        "  public constructor() N := 9; end constructor; "
        "end WidgetACF4170; "
        "class HolderACF4170 "
        "  public owned W: WidgetACF4170 := new(); "
        "end HolderACF4170; "
        "class ProgramACF4170 "
        "  public static method Main(): int "
        "    var h := new HolderACF4170(); "
        "    return h.W.N; "
        "  end Main; "
        "end ProgramACF4170;"),
        9);
}

BEZEL_TEST(sema_new_shorthand_enum_choice_construction_rejected_4170,
           accept_record_local_decl_shorthand_compiles_and_runs_spec_17_7) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public record PointACR4170(X: int, Y: int); "
        "class ProgramACR4170 "
        "  public static method Main(): int "
        "    var p: PointACR4170 := new(); "
        "    return p.X + p.Y; "
        "  end Main; "
        "end ProgramACR4170;"),
        0);
}

BEZEL_TEST(sema_new_shorthand_enum_choice_construction_rejected_4170,
           accept_record_field_initializer_shorthand_compiles_and_runs_spec_17_7) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public record PointACRF4170(X: int, Y: int); "
        "class HolderACRF4170 "
        "  public Slot: PointACRF4170 := new(); "
        "end HolderACRF4170; "
        "class ProgramACRF4170 "
        "  public static method Main(): int "
        "    var h := new HolderACRF4170(); "
        "    return h.Slot.X + h.Slot.Y; "
        "  end Main; "
        "end ProgramACRF4170;"),
        0);
}

BEZEL_TEST(sema_new_shorthand_enum_choice_construction_rejected_4170,
           accept_generic_class_local_decl_shorthand_compiles_and_runs_spec_17_7) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "class BoxACG4170<T> "
        "  public property V: T?; "
        "end BoxACG4170; "
        "class ProgramACG4170 "
        "  public static method Main(): int "
        "    var b: BoxACG4170<int> := new(); "
        "    return (if b.V <> null then 1 else 0); "
        "  end Main; "
        "end ProgramACG4170;"),
        0);
}

BEZEL_TEST(sema_new_shorthand_enum_choice_construction_rejected_4170,
           accept_nested_class_field_initializer_shorthand_compiles_and_runs_spec_17_7) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "class OuterACN4170 "
        "  public class InnerACN4170 "
        "    public property X: int; "
        "    public constructor() X := 3; end constructor; "
        "  end InnerACN4170; "
        "  public owned Slot: InnerACN4170 := new(); "
        "end OuterACN4170; "
        "class ProgramACN4170 "
        "  public static method Main(): int "
        "    var o := new OuterACN4170(); "
        "    return o.Slot.X; "
        "  end Main; "
        "end ProgramACN4170;"),
        3);
}

// Control: `new()` shorthand on an ENUM/CHOICE using the EXPLICIT spelling
// (`new ColorCTL4170(1)`) is the PRE-EXISTING, separately-fixed rejection
// (#960 / #4163) — this fix's shared-predicate refactor must not change
// its message or behavior.
BEZEL_TEST(sema_new_shorthand_enum_choice_construction_rejected_4170,
           control_explicit_new_enum_still_rejected_unchanged_spec_5_4_10) {
    auto r = runAot(
        "module M; "
        "public enum ColorCTL4170: int Red, Green; end ColorCTL4170; "
        "class ProgramCTL4170 "
        "  public static method Main(): int "
        "    var x := new ColorCTL4170(1); "
        "    return 0; "
        "  end Main; "
        "end ProgramCTL4170;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "`new` cannot construct an enum"));
}

BEZEL_TEST(sema_new_shorthand_enum_choice_construction_rejected_4170,
           control_explicit_new_choice_still_rejected_unchanged_spec_35_1) {
    auto r = runAot(
        "module M; "
        "public choice ChoiceCTL4170 Has(H: int), Empty(); end ChoiceCTL4170; "
        "class ProgramCTLC4170 "
        "  public static method Main(): int "
        "    var x := new ChoiceCTL4170(7); "
        "    return 0; "
        "  end Main; "
        "end ProgramCTLC4170;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "`new` cannot construct a choice"));
}

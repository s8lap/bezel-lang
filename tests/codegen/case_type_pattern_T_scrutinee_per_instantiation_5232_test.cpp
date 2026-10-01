// fix #5232 / §19.3 (v0.313): a case-pattern VERDICT over a TYPE PARAMETER
// scrutinee (`v: T`, the bare-T carrier shape) against a value-type pattern
// (`case v of int`, `case v of long`, `case v of Pt`, `case v of Color`)
// must be decided PER INSTANTIATION — the verdict reads the SUBSTITUTED type
// argument, the same rule #5195 installed for `is` / `as` / `as <value>`.
// §19.6 specifies the case-pattern verdict reads the scrutinee's static type
// after §7.4 narrowing.
//
// Pre-fix, `patternMatches`'s TypePattern branch (src/codegen/LlvmBackend_Exprs2.cpp)
// returned unconditional `builder_.getInt1(true)` whenever the scrutinee's
// static type was a bare type-parameter Named TypeRef (declaration==null) —
// the early-return primitive-vs-primitive check gated on `scrutTy->kind ==
// Primitive` and the bare-T form missed that gate. Likewise, the
// matchRecordOrEnumSym branch of the NamedPattern handler keyed
// `scrutTy->declaration == ts` against a null declaration.
//
// Together the two shapes admitted silently-wrong code:
//   * `case v of long` over `v: T` at T=int returns true (matches the long
//     arm and discards the `_` arm) — `Pick<int>(5)` returns 1 (the long
//     arm), should return 9 (the `_` arm).
//   * `case v of Pt` over `v: T` at T=Pt always falls through to `_`
//     (scrutTy->declaration is null, never == ts) — `Pick<Pt>(pt)` returns 9
//     instead of 1.
//   * `case v of Color` over `v: T` at T=Color ditto — `Pick<Color>(red)`
//     returns 9 instead of 1.
//
// The fix resolves `scrutTy` through the active monomorphization substitution
// (`genericSubstSemaTy_`) at the top of `patternMatches` and uses the
// resolved type at the two failing comparison sites. The substitution is
// already installed by `emitFunctionBody` for every instantiated body
// (`genericSubstSemaTy_[gp.name] = args[i]`); the unspecialized template
// keeps an empty map and the dead-body backstop already emits a well-formed
// never-match (so no template-body IR-verify failure from this change).
//
// Observable: each row prints ONE integer — the case-pattern arm result
// packed with `match*10 + nonmatch` for rows with two arms. With the fix the
// per-instantiation rule fires; without the fix the T=int row matches long
// (1) and the T=Pt / T=Color rows fall through to `_` (9) — pinning THIS
// defect, not just "some wrong arm".

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// `case v of long` over a generic method body — T=int and T=long probes.
// Long branch returns 1; `_` arm returns 9. Two-arm rows pack match/nonmatch
// as `match*10 + nonmatch` so a single row pins both legs.
const char* kLongBox =
    "module M; "
    "class C "
    "  public static method Pick<T>(v: T): int "
    "    return (case v of long => 1, _ => 9); "
    "  end Pick; "
    "end C; ";

// `case v of Pt` over a generic method body — T=Pt and T=Other probes.
// Pt-pattern arm returns 1; `_` arm returns 9.
const char* kRecBox =
    "module M; "
    "record Pt(x: int, y: int); "
    "record Other(a: int); "
    "class CR "
    "  public static method Pick<T>(v: T): int "
    "    return (case v of Pt => 1, _ => 9); "
    "  end Pick; "
    "end CR; ";

// `case v of Color` over a generic method body — T=Color and T=Flavor probes.
const char* kEnumBox =
    "module M; "
    "enum Color Red, Green; end Color; "
    "enum Flavor Sweet, Sour; end Flavor; "
    "class CE "
    "  public static method Pick<T>(v: T): int "
    "    return (case v of Color => 1, _ => 9); "
    "  end Pick; "
    "end CE; ";
}  // namespace

// §19.3 (#5232): `case v of long` over `v: T` at T=int must NOT match (int is
// not long) — the `_` arm returns 9. Without the fix the long arm fires (1).
BEZEL_TEST(case_type_pattern_T_scrutinee_per_instantiation_5232,
           primitive_int_instantiation_rejects_long_pattern) {
    BEZEL_EXPECT_EQ(run(std::string(kLongBox) +
        "class Program public static method Main(): int "
        "  return C.Pick<int>(5); "
        "end Main; end Program;"), 9);
}

// §19.3 (#5232): `case v of long` over `v: T` at T=long MUST match (long is
// long) — the long arm returns 1. Without the fix this row also returns 1
// (the same unconditional-true path), so it does not by itself pin the bug;
// paired with the int row above (which DOES flip 9↔1) the two together pin
// the per-instantiation rule. Pre-fix #5232: int row returns 1, long row
// returns 1 — both rows identical, per-instantiation rule not observed.
// Post-fix: int returns 9, long returns 1 — distinct, per instantiation.
BEZEL_TEST(case_type_pattern_T_scrutinee_per_instantiation_5232,
           primitive_long_instantiation_matches_long_pattern) {
    BEZEL_EXPECT_EQ(run(std::string(kLongBox) +
        "class Program public static method Main(): int "
        "  var x: long := 5; "
        "  return C.Pick<long>(x); "
        "end Main; end Program;"), 1);
}

// §19.3 (#5232): the same per-instantiation rule on a RECORD scrutinee —
// `case v of Pt` over `v: T` at T=Pt MUST match (Pt is Pt) — 1.
// Without the fix scrutTy->declaration is null (bare T), never == ts (Pt),
// so the record/enum branch returns false and the `_` arm fires (9).
BEZEL_TEST(case_type_pattern_T_scrutinee_per_instantiation_5232,
           record_same_instantiation_matches) {
    BEZEL_EXPECT_EQ(run(std::string(kRecBox) +
        "class Program public static method Main(): int "
        "  var p := new Pt(1, 2); "
        "  return CR.Pick<Pt>(p); "
        "end Main; end Program;"), 1);
}

// §19.3 (#5232): `case v of Pt` over `v: T` at T=Other must NOT match
// (Other is not Pt) — 9. Without the fix scrutTy->declaration is null,
// never == ts (Pt), so the record/enum branch always returns false (9)
// regardless of T's instantiation — silent wrong code for T=Pt (above) AND
// for T=Other (this row, accidentally correct).
BEZEL_TEST(case_type_pattern_T_scrutinee_per_instantiation_5232,
           record_other_instantiation_rejects_pt_pattern) {
    BEZEL_EXPECT_EQ(run(std::string(kRecBox) +
        "class Program public static method Main(): int "
        "  var o := new Other(3); "
        "  return CR.Pick<Other>(o); "
        "end Main; end Program;"), 9);
}

// §19.3 (#5232): per-instantiation rule on an ENUM scrutinee —
// `case v of Color` over `v: T` at T=Color MUST match (Color is Color) — 1.
// Without the fix scrutTy->declaration is null, never == ts (Color), the
// record/enum branch returns false and `_` fires (9).
BEZEL_TEST(case_type_pattern_T_scrutinee_per_instantiation_5232,
           enum_same_instantiation_matches) {
    BEZEL_EXPECT_EQ(run(std::string(kEnumBox) +
        "class Program public static method Main(): int "
        "  return CE.Pick<Color>(Color.Red); "
        "end Main; end Program;"), 1);
}

// §19.3 (#5232): `case v of Color` over `v: T` at T=Flavor must NOT match
// (Flavor is not Color) — 9.
BEZEL_TEST(case_type_pattern_T_scrutinee_per_instantiation_5232,
           enum_other_instantiation_rejects_color_pattern) {
    BEZEL_EXPECT_EQ(run(std::string(kEnumBox) +
        "class Program public static method Main(): int "
        "  return CE.Pick<Flavor>(Flavor.Sweet); "
        "end Main; end Program;"), 9);
}

// §19.3 (#5232): the per-instantiation rule distinguishes sibling primitive
// instantiations of one template — Pick<int>(5) returns 9, Pick<long>(5) returns 1
// at TWO instantiations of the SAME template body. The packed value 9*10+1 = 91
// pins BOTH legs in a single assertion. Pre-fix returns 1*10+1 = 11 (both
// match the long arm) — the bug's headlinenumber.
BEZEL_TEST(case_type_pattern_T_scrutinee_per_instantiation_5232,
           primitive_two_instantiations_distinct) {
    BEZEL_EXPECT_EQ(run(std::string(kLongBox) +
        "class Program public static method Main(): int "
        "  var l: long := 5; "
        "  return C.Pick<int>(5) * 10 + C.Pick<long>(l); "
        "end Main; end Program;"), 91);
}

// §19.3 (#5232): the per-instantiation rule distinguishes sibling record
// instantiations of one template — Pick<Pt>(pt) returns 1, Pick<Other>(o)
// returns 9 (Other is not Pt). Packed: 1*10+9 = 19. Pre-fix: 9*10+9 = 99
// (both fall through to `_`, the record/enum branch's null-declaration miss).
BEZEL_TEST(case_type_pattern_T_scrutinee_per_instantiation_5232,
           record_two_instantiations_distinct) {
    BEZEL_EXPECT_EQ(run(std::string(kRecBox) +
        "class Program public static method Main(): int "
        "  var p := new Pt(1, 2); "
        "  var o := new Other(3); "
        "  return CR.Pick<Pt>(p) * 10 + CR.Pick<Other>(o); "
        "end Main; end Program;"), 19);
}

// §19.3 (#5232): the per-instantiation rule distinguishes sibling enum
// instantiations of one template — Pick<Color>(red) returns 1,
// Pick<Flavor>(sweet) returns 9 (Flavor is not Color). Packed: 1*10+9 = 19.
// Pre-fix: 9*10+9 = 99.
BEZEL_TEST(case_type_pattern_T_scrutinee_per_instantiation_5232,
           enum_two_instantiations_distinct) {
    BEZEL_EXPECT_EQ(run(std::string(kEnumBox) +
        "class Program public static method Main(): int "
        "  return CE.Pick<Color>(Color.Red) * 10 + CE.Pick<Flavor>(Flavor.Sweet); "
        "end Main; end Program;"), 19);
}
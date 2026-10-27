// The `Math` / `Convert` declared surface lives in the ONE built-in
// parameter table (src/common/BuiltinRegistry.{hpp,cpp}).
//
// Campaign 2 (`builtin-call-admission`), the B3a lane. The anchor's own
// handback named the debt: "THE UN-FOLDED TYPE TABLES:
// `builtinStringBuilderSurface` (8 rows) and `builtinMathConvertSurface`
// (34 rows) in `src/sema/ExprTypecheck_Calls.cpp` are still INDEPENDENT
// parameter-type tables BESIDE the ONE table."
//
// This file pins the fold: the `Math` / `Convert` DECLARED FORMS are read out
// of `common::kBuiltinParams` — the same table the §22.5 name binder, the
// §5.1 admission gate and the §7.3 nullability gate read — so a form's
// parameter NAME (§22.5), parameter TYPE (§5.1) and parameter NULLABILITY
// (§7.3) cannot disagree.
//
// The rows below are transcribed from stdlib §2.2's and §2.3's own
// declarations (v0.343):
//
//   §2.2: `Abs(x: int): int` / `Abs(x: long): long` / `Abs(x: double):
//          double` / `Abs(x: decimal): decimal`;
//         `Sqrt(x: double): double`; `Pow(x: double, y: double): double`;
//         `Floor(x: double): double` / `Floor(x: decimal): decimal`;
//         `Ceiling` and `Truncate` in the same pair;
//         `Round(x: double, digits: int := 0): double`;
//         `Fmod(x: double, y: double): double`;
//         `IEEERemainder(x: double, y: double): double`;
//         `Min`/`Max`/`Clamp`/`Sign` over a TYPE PARAMETER.
//   §2.3: `ToInt32(value: string): int` and its siblings;
//         `ToString(value: <primitive>): string` (one form per primitive)
//         and `ToString(value: <int|long>, toBase: int): string`;
//         the `TryTo*` family over a `string` value and an `out` slot.
//
// per §22.5 / §22.6 / §5.1 / §7.3, stdlib §2.2 / §2.3

#include "test_harness.hpp"

#include "common/BuiltinRegistry.hpp"

#include <string>
#include <vector>

using namespace bezel::common;

namespace {

// The declared TYPE at one position of a form, spelled by the table's own
// bit set.
std::string typeAt(const BuiltinParamForm& f, std::size_t p) {
    if (p >= f.kinds.size()) return "";
    switch (f.kinds[p]) {
        case kBPTString: return "string";
        case kBPTInt: return "int";
        case kBPTLong: return "long";
        case kBPTFloat: return "float";
        case kBPTDouble: return "double";
        case kBPTDecimal: return "decimal";
        case kBPTBool: return "bool";
        case kBPTOut: return "out";
        default: return "";
    }
}

// The declared forms of `head.member` at `argCount` arguments, as
// "<type0>, <type1>" — the shape of a form's parameter TYPES.
std::vector<std::string> typeLists(std::string_view head,
                                    std::string_view member,
                                    std::size_t argCount) {
    std::vector<std::string> out;
    for (const auto& f : builtinParamForms(head, member, argCount)) {
        std::string s;
        for (std::size_t p = 0; p < f.total; ++p) {
            if (p) s += ", ";
            s += typeAt(f, p);
        }
        out.push_back(s);
    }
    return out;
}

}  // namespace

// `Math.Abs` declares FOUR forms (int / long / double / decimal), and §22.6's
// ranking needs them SEPARATED — so they are four ROWS of the one table, one
// per declared form.
BEZEL_TEST(builtin_registry_math_convert_surface,
           math_abs_declares_four_forms_with_their_types_2_2) {
    auto got = typeLists("Math", "Abs", 1);
    BEZEL_EXPECT_EQ(got.size(), static_cast<std::size_t>(4));
    if (got.size() == 4) {
        BEZEL_EXPECT_TRUE(got[0] == "int");
        BEZEL_EXPECT_TRUE(got[1] == "long");
        BEZEL_EXPECT_TRUE(got[2] == "double");
        BEZEL_EXPECT_TRUE(got[3] == "decimal");
    }
}

// The DECLARED SIGNATURE is the standard library's own words — the text a
// diagnostic quotes.
BEZEL_TEST(builtin_registry_math_convert_surface,
           math_abs_forms_carry_the_declared_signatures_2_2) {
    auto forms = builtinParamForms("Math", "Abs", 1);
    BEZEL_EXPECT_EQ(forms.size(), static_cast<std::size_t>(4));
    if (forms.size() == 4) {
        BEZEL_EXPECT_TRUE(forms[0].sig == "Abs(x: int): int");
        BEZEL_EXPECT_TRUE(forms[1].sig == "Abs(x: long): long");
        BEZEL_EXPECT_TRUE(forms[2].sig == "Abs(x: double): double");
        BEZEL_EXPECT_TRUE(forms[3].sig == "Abs(x: decimal): decimal");
    }
}

// The rounding family declares a `double` AND a `decimal` form, and §22.6's
// ambiguity rule is exactly about that pair (the v0.228 migration note).
BEZEL_TEST(builtin_registry_math_convert_surface,
           math_rounding_family_declares_double_and_decimal_2_2) {
    for (const char* m : {"Floor", "Ceiling", "Truncate"}) {
        auto got = typeLists("Math", m, 1);
        BEZEL_EXPECT_EQ(got.size(), static_cast<std::size_t>(2));
        if (got.size() == 2) {
            BEZEL_EXPECT_TRUE(got[0] == "double");
            BEZEL_EXPECT_TRUE(got[1] == "decimal");
        }
    }
}

// `Round` declares ONE method with a DEFAULTED second parameter
// (`digits: int := 0`), so the table carries it in the ARITY-EXPANDED form:
// one row per arity the call may have.
BEZEL_TEST(builtin_registry_math_convert_surface,
           math_round_is_arity_expanded_over_its_default_2_2) {
    auto one = typeLists("Math", "Round", 1);
    BEZEL_EXPECT_EQ(one.size(), static_cast<std::size_t>(1));
    if (one.size() == 1) BEZEL_EXPECT_TRUE(one[0] == "double");
    auto two = typeLists("Math", "Round", 2);
    BEZEL_EXPECT_EQ(two.size(), static_cast<std::size_t>(1));
    if (two.size() == 1) BEZEL_EXPECT_TRUE(two[0] == "double, int");
}

// The generic selectors declare a TYPE PARAMETER, so their parameter has NO
// concrete type: the table must carry NO type opinion there (the constraint,
// not §5.1, governs the argument).
BEZEL_TEST(builtin_registry_math_convert_surface,
           math_generic_selectors_carry_no_parameter_type_2_2) {
    for (const char* m : {"Min", "Max"}) {
        auto got = typeLists("Math", m, 2);
        BEZEL_EXPECT_EQ(got.size(), static_cast<std::size_t>(1));
        if (got.size() == 1) BEZEL_EXPECT_TRUE(got[0] == ", ");
    }
    auto clamp = typeLists("Math", "Clamp", 3);
    BEZEL_EXPECT_EQ(clamp.size(), static_cast<std::size_t>(1));
    if (clamp.size() == 1) BEZEL_EXPECT_TRUE(clamp[0] == ", , ");
}

// `Convert.ToString` declares one form per numeric primitive and one for
// `bool`; the `value` position's types are the SIX of them.
BEZEL_TEST(builtin_registry_math_convert_surface,
           convert_tostring_declares_one_form_per_primitive_2_3) {
    auto got = typeLists("Convert", "ToString", 1);
    BEZEL_EXPECT_EQ(got.size(), static_cast<std::size_t>(6));
    if (got.size() == 6) {
        BEZEL_EXPECT_TRUE(got[0] == "int");
        BEZEL_EXPECT_TRUE(got[1] == "long");
        BEZEL_EXPECT_TRUE(got[2] == "float");
        BEZEL_EXPECT_TRUE(got[3] == "double");
        BEZEL_EXPECT_TRUE(got[4] == "decimal");
        BEZEL_EXPECT_TRUE(got[5] == "bool");
    }
}

// The two-argument `ToString` forms declare `value: int` and `value: long`
// with a `toBase: int` at the second position.
BEZEL_TEST(builtin_registry_math_convert_surface,
           convert_tostring_two_argument_forms_declare_the_radix_2_3) {
    auto got = typeLists("Convert", "ToString", 2);
    BEZEL_EXPECT_EQ(got.size(), static_cast<std::size_t>(2));
    if (got.size() == 2) {
        BEZEL_EXPECT_TRUE(got[0] == "int, int");
        BEZEL_EXPECT_TRUE(got[1] == "long, int");
    }
}

// The `TryTo*` family declares a `string` value and an `out` slot, whose type
// the out-variable machinery owns (§22.3).
BEZEL_TEST(builtin_registry_math_convert_surface,
           convert_tryto_family_declares_a_string_value_and_an_out_slot_2_3) {
    for (const char* m : {"TryToInt32", "TryToInt64", "TryToDouble",
                           "TryToDecimal", "TryToBoolean"}) {
        auto got = typeLists("Convert", m, 2);
        BEZEL_EXPECT_EQ(got.size(), static_cast<std::size_t>(1));
        if (got.size() == 1) BEZEL_EXPECT_TRUE(got[0] == "string, out");
    }
}

// The §7.3 NULLABILITY of a declared form rides in the SAME row as its
// types: the `Math` positions are non-nullable, and the `out` slot of the
// `TryTo*` family is NOT one of them.
BEZEL_TEST(builtin_registry_math_convert_surface,
           math_positions_are_non_nullable_and_the_out_slot_is_not_7_3) {
    auto abs = builtinParamForms("Math", "Abs", 1);
    BEZEL_EXPECT_TRUE(!abs.empty());
    if (!abs.empty()) BEZEL_EXPECT_TRUE((abs[0].nonNullable & 0x01) != 0);
    auto pow = builtinParamForms("Math", "Pow", 2);
    BEZEL_EXPECT_TRUE(!pow.empty());
    if (!pow.empty()) BEZEL_EXPECT_TRUE((pow[0].nonNullable & 0x03) == 0x03);
    auto tryTo = builtinParamForms("Convert", "TryToInt32", 2);
    BEZEL_EXPECT_TRUE(!tryTo.empty());
    if (!tryTo.empty()) BEZEL_EXPECT_TRUE((tryTo[0].nonNullable & 0x01) != 0);
    if (!tryTo.empty()) BEZEL_EXPECT_TRUE((tryTo[0].nonNullable & 0x02) == 0);
}

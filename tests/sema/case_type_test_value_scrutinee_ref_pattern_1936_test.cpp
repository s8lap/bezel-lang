// Spec-derived tests for §19.5/§19.3 (#1936): a typed binding pattern
// `var x: T` whose type T is a REFERENCE type (class, interface, string, or a
// BUILT-IN exception) against a VALUE-typed scrutinee (a primitive, a record, or
// an enum) can NEVER match — a value type is never a reference. Such an arm must
// be REJECTED at admission with a clear diagnostic (§19.5), the same posture the
// #1203 check already applied to the primitive-scrutinee-vs-class/interface case.
//
// #1936 was that #1203 check missing two shapes, each of which reached codegen
// and CRASHED on a nonsensical program rather than being rejected:
//   (a) a BUILT-IN exception pattern (`case 5 of var x: IOException`) — the
//       built-in carries no user TypeSymbol, so its binding TypeRef has a null
//       `declaration` and the declaration-gated class/interface test missed it.
//       Codegen's typed-binding built-in-exception branch is guarded on a POINTER
//       scrutinee, so a value (non-pointer) scrutinee fell through to a `return
//       nullptr` that left `case.end` unterminated → malformed IR (a hard abort).
//   (b) a RECORD or ENUM scrutinee against ANY reference pattern (`case p of
//       var q: C` over a record `p`) — the #1203 check only recognised a
//       *primitive* value scrutinee, so a record/enum value scrutinee reached
//       codegen's emitIsA, which ICmp'd the non-pointer struct/i32 against a null
//       pointer and ABORTED the backend (ICmpInst assertion).
//
// The rejection must fire ONLY on a genuinely-impossible value-vs-reference pair;
// legitimate reference-scrutinee type tests (a caught Exception against a
// built-in exception pattern, a base-typed value against a subclass pattern, a
// string scrutinee against a `var x: string` binding) must all stay ACCEPTED.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff an ERROR diagnostic mentions the #1936/#1203 value-vs-reference
// reason — pins THIS defect, not merely "some error".
bool hasRefTypeBindError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find(
                "value-type scrutinee can never match a reference-typed") !=
                std::string::npos)
            return true;
    }
    return false;
}

// Shared preamble: a class, an interface, a record, and an enum to draw
// reference patterns and value scrutinees from.
constexpr const char* kDecls =
    "module M;\n"
    "class C public method M(): int return 0; end M; end C;\n"
    "interface I method M(): int; end I;\n"
    "record Pt(x: int, y: int);\n"
    "enum Color Red, Green; end Color;\n";

std::string prog(const std::string& body) {
    return std::string(kDecls) + "class Program\n" + body + "end Program;\n";
}

}  // namespace

// ---- FAIL-without / PASS-with: value scrutinee vs reference pattern REJECTED --

// (1) The issue's exact repro: `case 5 of var x: IOException`. Without the fix
// this reached codegen and emitted malformed IR (unterminated case.end). Now the
// arm is rejected at admission (§19.5): an int is never an IOException.
BEZEL_TEST(case_type_test_value_scrutinee_ref_pattern_1936,
           int_vs_builtin_exception_rejected) {
    auto r = parseSemaAll(prog(
        "  public static method Main(): int\n"
        "    return case 5 of var x: IOException => 1, _ => 9 end;\n"
        "  end Main;\n"));
    BEZEL_EXPECT_TRUE(hasRefTypeBindError(r));
}

// (2) int vs a base built-in exception `Exception`.
BEZEL_TEST(case_type_test_value_scrutinee_ref_pattern_1936,
           int_vs_base_exception_rejected) {
    auto r = parseSemaAll(prog(
        "  public static method Main(): int\n"
        "    return case 5 of var x: Exception => 1, _ => 9 end;\n"
        "  end Main;\n"));
    BEZEL_EXPECT_TRUE(hasRefTypeBindError(r));
}

// (3) record scrutinee vs a class pattern. Without the fix: ICmpInst assertion
// abort in emitIsA (a struct value ICmp'd against a null pointer).
BEZEL_TEST(case_type_test_value_scrutinee_ref_pattern_1936,
           record_vs_class_rejected) {
    auto r = parseSemaAll(prog(
        "  public static method Main(): int\n"
        "    var p := new Pt(1, 2);\n"
        "    return case p of var q: C => 1, _ => 9 end;\n"
        "  end Main;\n"));
    BEZEL_EXPECT_TRUE(hasRefTypeBindError(r));
}

// (4) record scrutinee vs an interface pattern (same emitIsA crash).
BEZEL_TEST(case_type_test_value_scrutinee_ref_pattern_1936,
           record_vs_interface_rejected) {
    auto r = parseSemaAll(prog(
        "  public static method Main(): int\n"
        "    var p := new Pt(1, 2);\n"
        "    return case p of var q: I => 1, _ => 9 end;\n"
        "  end Main;\n"));
    BEZEL_EXPECT_TRUE(hasRefTypeBindError(r));
}

// (5) record scrutinee vs a built-in exception pattern (malformed IR without fix).
BEZEL_TEST(case_type_test_value_scrutinee_ref_pattern_1936,
           record_vs_builtin_exception_rejected) {
    auto r = parseSemaAll(prog(
        "  public static method Main(): int\n"
        "    var p := new Pt(1, 2);\n"
        "    return case p of var q: IOException => 1, _ => 9 end;\n"
        "  end Main;\n"));
    BEZEL_EXPECT_TRUE(hasRefTypeBindError(r));
}

// (6) enum scrutinee vs a class pattern (ICmpInst assertion abort without fix).
BEZEL_TEST(case_type_test_value_scrutinee_ref_pattern_1936,
           enum_vs_class_rejected) {
    auto r = parseSemaAll(prog(
        "  public static method Main(): int\n"
        "    var c := Color.Red;\n"
        "    return case c of var q: C => 1, _ => 9 end;\n"
        "  end Main;\n"));
    BEZEL_EXPECT_TRUE(hasRefTypeBindError(r));
}

// (7) enum scrutinee vs a built-in exception pattern (malformed IR without fix).
BEZEL_TEST(case_type_test_value_scrutinee_ref_pattern_1936,
           enum_vs_builtin_exception_rejected) {
    auto r = parseSemaAll(prog(
        "  public static method Main(): int\n"
        "    var c := Color.Red;\n"
        "    return case c of var q: IOException => 1, _ => 9 end;\n"
        "  end Main;\n"));
    BEZEL_EXPECT_TRUE(hasRefTypeBindError(r));
}

// (8) STATEMENT position (the second admission site — CaseStmt): a record
// scrutinee vs a built-in exception pattern. Same defect, different site.
BEZEL_TEST(case_type_test_value_scrutinee_ref_pattern_1936,
           stmt_record_vs_builtin_exception_rejected) {
    auto r = parseSemaAll(prog(
        "  public static method Main(): int\n"
        "    var p := new Pt(1, 2);\n"
        "    var n := 0;\n"
        "    case p of var q: IOException => n := 1, _ => n := 9 end;\n"
        "    return n;\n"
        "  end Main;\n"));
    BEZEL_EXPECT_TRUE(hasRefTypeBindError(r));
}

// (9) STATEMENT position: int scrutinee vs a built-in exception pattern.
BEZEL_TEST(case_type_test_value_scrutinee_ref_pattern_1936,
           stmt_int_vs_builtin_exception_rejected) {
    auto r = parseSemaAll(prog(
        "  public static method Main(): int\n"
        "    var n := 0;\n"
        "    case 5 of var x: IOException => n := 1, _ => n := 9 end;\n"
        "    return n;\n"
        "  end Main;\n"));
    BEZEL_EXPECT_TRUE(hasRefTypeBindError(r));
}

// ---- No over-rejection: legitimate reference type tests stay ACCEPTED --------

// (10) A caught Exception scrutinee against a built-in exception pattern — the
// #1854 idiom. A reference scrutinee CAN be a built-in exception; must compile
// clean (no value-vs-reference rejection).
BEZEL_TEST(case_type_test_value_scrutinee_ref_pattern_1936,
           exception_scrutinee_vs_builtin_exception_accepted) {
    auto r = parseSemaAll(
        "module M;\n"
        "class Program\n"
        "  public static method Main(): int\n"
        "    var r := 0;\n"
        "    try throw new InvalidOperationException(\"x\");\n"
        "    catch e: Exception =>\n"
        "      r := case e of var x: InvalidOperationException => 3, _ => 0 end;\n"
        "    end;\n"
        "    return r;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(hasRefTypeBindError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (11) A string scrutinee against a `var x: string` binding — string is a
// reference type, so this is a legitimate (always-non-null) type test, NOT a
// value-vs-reference impossibility.
BEZEL_TEST(case_type_test_value_scrutinee_ref_pattern_1936,
           string_scrutinee_vs_string_binding_accepted) {
    auto r = parseSemaAll(prog(
        "  public static method Take(s: string): int\n"
        "    return case s of var x: string => 1, _ => 9 end;\n"
        "  end Take;\n"));
    BEZEL_EXPECT_FALSE(hasRefTypeBindError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (12) A base-class scrutinee against a subclass pattern — a genuine downcast
// type test. Must stay ACCEPTED.
BEZEL_TEST(case_type_test_value_scrutinee_ref_pattern_1936,
           base_scrutinee_vs_subclass_accepted) {
    auto r = parseSemaAll(
        "module M;\n"
        "class Animal public method Speak(): int return 0; end Speak; end Animal;\n"
        "class Dog : Animal public method Bark(): int return 1; end Bark; end Dog;\n"
        "class Program\n"
        "  public static method Take(a: Animal): int\n"
        "    return case a of var d: Dog => 1, _ => 9 end;\n"
        "  end Take;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(hasRefTypeBindError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (13) A value scrutinee against a VALUE pattern of the same type — `var x: int`
// over an int — is a legitimate always-satisfied binding. Must NOT be rejected
// (the rejection targets reference patterns only).
BEZEL_TEST(case_type_test_value_scrutinee_ref_pattern_1936,
           int_vs_int_binding_accepted) {
    auto r = parseSemaAll(prog(
        "  public static method Main(): int\n"
        "    return case 5 of var x: int => x, _ => 9 end;\n"
        "  end Main;\n"));
    BEZEL_EXPECT_FALSE(hasRefTypeBindError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (14) A record scrutinee against a DESTRUCTURING pattern of its own type — the
// ordinary record match. Not a typed binding, so the new check never fires; must
// stay ACCEPTED (guards against over-rejecting record scrutinees wholesale).
BEZEL_TEST(case_type_test_value_scrutinee_ref_pattern_1936,
           record_destructure_own_type_accepted) {
    auto r = parseSemaAll(prog(
        "  public static method Main(): int\n"
        "    var p := new Pt(1, 2);\n"
        "    return case p of Pt(a, b) => a + b, _ => 9 end;\n"
        "  end Main;\n"));
    BEZEL_EXPECT_FALSE(hasRefTypeBindError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

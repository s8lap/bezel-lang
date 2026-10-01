// Spec-derived tests for #5239: §12.7 "Value-typed operands" (v0.313) reads,
// for a carrier type-narrowed by that section, its DECLARED type, "so a
// narrowed `Object` or interface carrier stays a boxed reference operand".
// After `if o is int then`, a reference type pattern (`string`, an interface,
// bare or binding) over `o` is therefore NOT a value-type scrutinee against a
// reference pattern (§19.3 / §19.5) and must be admitted, in both the case
// expression and the case statement forms. An UN-narrowed value-typed
// scrutinee (`var i: int`) against the same patterns is still rejected.

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

bool hasValueScrutRefPatternError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("a value-type scrutinee can never match a "
                           "reference-typed") != std::string::npos)
            return true;
    }
    return false;
}

bool hasAnyError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error) return true;
    return false;
}

std::string prog(const std::string& body) {
    return "module M;\n"
           "interface IA method M(): int; end IA;\n"
           "class C\n"
           "  public static method Main(): int\n" + body +
           "  end Main;\n"
           "end C;\n";
}

}  // namespace

// ---- ACCEPT: narrowed Object carrier, declared type is a reference --------

BEZEL_TEST(case_narrowed_object_ref_pattern_5239, narrowed_bare_string_admitted) {
    auto r = parseSemaAll(prog(
        "    var o: Object := 5;\n"
        "    if o is int then\n"
        "      return (case o of string => 4, _ => 0);\n"
        "    end if;\n"
        "    return 0;\n"));
    BEZEL_EXPECT_FALSE(hasValueScrutRefPatternError(r));
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

BEZEL_TEST(case_narrowed_object_ref_pattern_5239, narrowed_bare_interface_admitted) {
    auto r = parseSemaAll(prog(
        "    var o: Object := 5;\n"
        "    if o is int then\n"
        "      return (case o of IA => 1, _ => 0);\n"
        "    end if;\n"
        "    return 0;\n"));
    BEZEL_EXPECT_FALSE(hasValueScrutRefPatternError(r));
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

BEZEL_TEST(case_narrowed_object_ref_pattern_5239, narrowed_bare_interface_and_object_admitted) {
    auto r = parseSemaAll(prog(
        "    var o: Object := 5;\n"
        "    if o is int then\n"
        "      return (case o of IA => 1, Object => 3, _ => 0);\n"
        "    end if;\n"
        "    return 0;\n"));
    BEZEL_EXPECT_FALSE(hasValueScrutRefPatternError(r));
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

BEZEL_TEST(case_narrowed_object_ref_pattern_5239, narrowed_binding_string_and_interface_admitted) {
    auto r = parseSemaAll(prog(
        "    var o: Object := 5;\n"
        "    if o is int then\n"
        "      return (case o of var s: string => 1, var a: IA => 2, _ => 0);\n"
        "    end if;\n"
        "    return 0;\n"));
    BEZEL_EXPECT_FALSE(hasValueScrutRefPatternError(r));
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

BEZEL_TEST(case_narrowed_object_ref_pattern_5239, stmt_narrowed_bare_and_binding_admitted) {
    auto r = parseSemaAll(prog(
        "    var o: Object := 5;\n"
        "    var n := 0;\n"
        "    if o is int then\n"
        "      case o of\n"
        "        string => n := 1,\n"
        "        IA => n := 2,\n"
        "        var s: string => n := 3,\n"
        "        _ => n := 4\n"
        "      end case;\n"
        "    end if;\n"
        "    return n;\n"));
    BEZEL_EXPECT_FALSE(hasValueScrutRefPatternError(r));
}

// ---- REJECT controls: un-narrowed value scrutinee is still rejected -------

BEZEL_TEST(case_narrowed_object_ref_pattern_5239, int_scrutinee_bare_string_still_rejected) {
    auto r = parseSemaAll(prog(
        "    var i: int := 5;\n"
        "    return (case i of string => 4, _ => 0);\n"));
    BEZEL_EXPECT_TRUE(hasValueScrutRefPatternError(r));
}

BEZEL_TEST(case_narrowed_object_ref_pattern_5239, int_scrutinee_bare_interface_still_rejected) {
    auto r = parseSemaAll(prog(
        "    var i: int := 5;\n"
        "    return (case i of IA => 1, _ => 0);\n"));
    BEZEL_EXPECT_TRUE(hasValueScrutRefPatternError(r));
}

BEZEL_TEST(case_narrowed_object_ref_pattern_5239, int_scrutinee_binding_string_still_rejected) {
    auto r = parseSemaAll(prog(
        "    var i: int := 5;\n"
        "    return (case i of var s: string => 1, _ => 0);\n"));
    BEZEL_EXPECT_TRUE(hasValueScrutRefPatternError(r));
}

BEZEL_TEST(case_narrowed_object_ref_pattern_5239, stmt_int_scrutinee_bare_string_still_rejected) {
    auto r = parseSemaAll(prog(
        "    var i: int := 5;\n"
        "    var n := 0;\n"
        "    case i of string => n := 1, _ => n := 2 end case;\n"
        "    return n;\n"));
    BEZEL_EXPECT_TRUE(hasValueScrutRefPatternError(r));
}

// Spec-derived tests for §19.3/§19.5/§5.1/§12.9 (#1913): the ADMISSION of a
// typed binding pattern `var x: T` whose generic INSTANTIATION is incompatible
// with the scrutinee.
//
// A typed binding pattern is a type-test-plus-bind that binds the matched value
// at the STATIC type `T`. The runtime type-test erases generic arguments (it
// checks only the class tag), so an incompatible generic instantiation
// (`var x: Ok<string>` at scrutinee `Result<int>`) would match at runtime and
// bind an `Ok<int>` value as `Ok<string>` — a type-safety hole, since
// `Result<int>` is not assignable to `Result<string>` (§5.1) and class
// hierarchies are invariant (§12.9). The pattern must therefore be REJECTED at
// admission, the counterpart of the arg-SENSITIVE §19.6 coverage the #1874
// exhaustiveness fix installed (this issue was #1874's "open question #1").
//
// The rejection must fire ONLY when incompatibility is provable on ground
// structure — a wrong instantiation of a related CLASS declaration under
// invariance. An EXACT instantiation, a legal upcast/downcast, a rigid
// enclosing-body type parameter (a type pattern is tested per instantiation,
// §19.3 v0.182), and a non-generic hierarchy must all stay ACCEPTED (no
// over-rejection — the concern #1874 flags via variantGenericAssignable).

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

// True iff an ERROR diagnostic mentions the #1913 wrong-instantiation reason —
// pins THIS defect, not merely "some error".
bool hasInstantiationError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("generic instantiation can never match") !=
                std::string::npos)
            return true;
    }
    return false;
}

// The shared closed generic hierarchy (§19.6's determined shape):
//   sealed abstract class Result<T>; sealed class Ok<T>: Result<T>; Err<T>: …
constexpr const char* kHierarchy =
    "module M;\n"
    "sealed abstract class Result<T> end Result;\n"
    "sealed class Ok<T> : Result<T> end Ok;\n"
    "sealed class Err<T> : Result<T> end Err;\n";

std::string prog(const std::string& body) {
    return std::string(kHierarchy) +
           "class Program\n" + body + "end Program;\n";
}

}  // namespace

// ---- FAIL-without / PASS-with: incompatible instantiations are REJECTED ----

// (1) The issue's repro, EXPRESSION position: scrutinee `Result<int>`, arm
// `var x: Ok<string>` names a DESCENDANT (`Ok: Result`) at the WRONG argument.
// `Ok<string>` projects up to `Result<string>`, not `Result<int>` — statically
// impossible under invariance, so the bind at `Ok<string>` is rejected.
// FAILS WITHOUT the fix (admission accepted `Ok<string>` by declaration only).
BEZEL_TEST(case_type_test_instantiation_1913,
           expr_descendant_wrong_instantiation_rejected) {
    auto r = parseSemaAll(prog(
        "  public method M(r: Result<int>): int\n"
        "    var n := case r of\n"
        "      var x: Ok<string> => 1,\n"
        "      _ => 0\n"
        "    end;\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasInstantiationError(r));
}

// (2) Same repro, STATEMENT position (the second admission site — CaseStmt).
BEZEL_TEST(case_type_test_instantiation_1913,
           stmt_descendant_wrong_instantiation_rejected) {
    auto r = parseSemaAll(prog(
        "  public method M(r: Result<int>): int\n"
        "    var n := 0;\n"
        "    case r of\n"
        "      var x: Ok<string> => n := 1,\n"
        "      _ => n := 2\n"
        "    end;\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasInstantiationError(r));
}

// (3) SAME-declaration wrong instantiation: scrutinee `Result<int>`, arm
// `var x: Result<string>` names the scrutinee's OWN (abstract) class at the
// wrong argument — invariant, so no value of `Result<int>` is a `Result<string>`.
BEZEL_TEST(case_type_test_instantiation_1913,
           same_declaration_wrong_instantiation_rejected) {
    auto r = parseSemaAll(prog(
        "  public method M(r: Result<int>): int\n"
        "    var n := case r of\n"
        "      var x: Result<string> => 1,\n"
        "      _ => 0\n"
        "    end;\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasInstantiationError(r));
}

// (4) UPCAST at a wrong instantiation: scrutinee `Ok<int>`, arm
// `var x: Result<string>` names an ANCESTOR at the wrong argument. `Ok<int>`
// projects up to `Result<int>` ≠ `Result<string>` — impossible.
BEZEL_TEST(case_type_test_instantiation_1913,
           upcast_wrong_instantiation_rejected) {
    auto r = parseSemaAll(prog(
        "  public method M(o: Ok<int>): int\n"
        "    var n := case o of\n"
        "      var x: Result<string> => 1,\n"
        "      _ => 0\n"
        "    end;\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasInstantiationError(r));
}

// ---- No over-rejection: legitimate patterns stay ACCEPTED (controls) ----

// (5) EXACT descendant instantiation: scrutinee `Result<int>`, arm
// `var x: Ok<int>` — the §19.6 determined-shape idiom. Must compile clean.
BEZEL_TEST(case_type_test_instantiation_1913,
           exact_descendant_instantiation_accepted) {
    auto r = parseSemaAll(prog(
        "  public method M(r: Result<int>): int\n"
        "    var n := case r of\n"
        "      var x: Ok<int> => 1,\n"
        "      var y: Err<int> => 2,\n"
        "      _ => 0\n"
        "    end;\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_FALSE(hasInstantiationError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (6) Legal UPCAST at the correct instantiation: scrutinee `Ok<int>`, arm
// `var x: Result<int>` — `Ok<int>` projects to `Result<int>`. Must stay ACCEPTED.
BEZEL_TEST(case_type_test_instantiation_1913,
           upcast_correct_instantiation_accepted) {
    auto r = parseSemaAll(prog(
        "  public method M(o: Ok<int>): int\n"
        "    var n := case o of\n"
        "      var x: Result<int> => 1,\n"
        "      _ => 0\n"
        "    end;\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_FALSE(hasInstantiationError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (7) SAME-declaration EXACT instantiation: scrutinee `Result<int>`, arm
// `var x: Result<int>` — identity. Must stay ACCEPTED.
BEZEL_TEST(case_type_test_instantiation_1913,
           same_declaration_exact_accepted) {
    auto r = parseSemaAll(prog(
        "  public method M(r: Result<int>): int\n"
        "    var n := case r of\n"
        "      var x: Result<int> => 1,\n"
        "      _ => 0\n"
        "    end;\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_FALSE(hasInstantiationError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (8) RIGID enclosing-body parameter: in a generic method the scrutinee is
// `Result<U>` (U rigid). Arm `var x: Ok<int>` is admissible at the instantiation
// U := int (a type pattern is tested per instantiation, §19.3 v0.182), so it must
// NOT be rejected at the declaration — variantGenericAssignable treats the rigid
// U as compatible. (This is the no-over-rejection direction #1874 emphasizes.)
BEZEL_TEST(case_type_test_instantiation_1913,
           rigid_scrutinee_param_not_over_rejected) {
    auto r = parseSemaAll(prog(
        "  public method Rig<U>(r: Result<U>): int\n"
        "    var n := case r of\n"
        "      var x: Ok<int> => 1,\n"
        "      _ => 0\n"
        "    end;\n"
        "    return n;\n"
        "  end Rig;\n"));
    BEZEL_EXPECT_FALSE(hasInstantiationError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (9) NON-generic hierarchy: scrutinee `Shape`, arm `var x: Circle` — no generic
// instantiation to compare, so the new check never fires. Must stay ACCEPTED.
BEZEL_TEST(case_type_test_instantiation_1913,
           non_generic_hierarchy_unaffected) {
    auto r = parseSemaAll(
        "module M;\n"
        "sealed abstract class Shape end Shape;\n"
        "sealed class Circle : Shape end Circle;\n"
        "sealed class Square : Shape end Square;\n"
        "class Program\n"
        "  public method M(s: Shape): int\n"
        "    var n := case s of\n"
        "      var c: Circle => 1,\n"
        "      var q: Square => 2\n"
        "    end;\n"
        "    return n;\n"
        "  end M;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(hasInstantiationError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

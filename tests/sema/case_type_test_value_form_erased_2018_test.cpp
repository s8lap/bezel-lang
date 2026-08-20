// Spec-derived tests for §12.7/§2.8.3 rule 4/§17.4 (#2018, v0.197): the erasure guard on
// the VALUE/expression `case` form's binding type-test path for a PLAIN
// (non-generic-interface) scrutinee (src/sema/ExprTypecheck_Exprs.cpp — the
// CaseExpr handler in `visitChildren`).
//
// This is the value-form sibling of #2012 (which fixed the STATEMENT-form CaseStmt
// handler in ExprTypecheck.cpp) and #2006 (which fixed the plain `is`/`as`
// expression path in THIS file). Before #2018 the value-form
// `var n := case o of var x: Wrap<(string)->string> => 1, _ => 0` reached the
// COLLAPSED per-monomorphization descriptor with NO guard for a PLAIN scrutinee
// (Object, a class, a NON-generic interface): `appendDescKey` does not encode a
// function type's identity (Kind::Function -> "F" at any depth), so a
// `Wrap<(int)->int>` value FALSE-POSITIVE-matched the arm and was bound at the
// wrong static type — the same memory-unsafe type confusion #2001/#2006/#2012
// fixed. #2018 brings the value form to PARITY: a case binding to a CONSTRUCTED
// generic target whose type argument is runtime-ERASED (a function type, ...) and
// that carries no type variable is REJECTED at compile time. Reified targets stay
// ACCEPTED — post-#2001 a folded inline-array argument is runtime-distinguishable,
// and a bare type parameter resolves to a concrete argument at monomorphization
// (#1843) — so neither is over-rejected. A generic-INTERFACE scrutinee stays the
// domain of `typeTestInterfaceBindAmbiguous` (rejected there, once, not double-
// reported; its sound unique-conformer admission is not overridden).
//
// NB: these use the VALUE/expression form `var n := case ... => <expr>` (a
// CaseExpr), the construct the ExprTypecheck_Exprs.cpp handler under test lowers.

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

// True iff an ERROR diagnostic carries the #2018 PLAIN-scrutinee erasure reason.
// The phrase "runtime type descriptor collapses" is unique to this guard (shared
// with #2012's statement-form wording) — the generic-INTERFACE #1946 diagnostic
// instead says "another instantiation of the pattern class", so the two remain
// distinguishable in a program that has both.
bool hasValueCaseErasedError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("runtime type descriptor collapses") !=
                std::string::npos)
            return true;
    }
    return false;
}

// Count ERROR diagnostics mentioning "not runtime-distinguishable" (the shared
// erasure-class marker of BOTH this guard and the #1946 interface guard) — used
// to prove the generic-interface case is reported EXACTLY once (no double-report).
int countErasureErrors(const bezel::test::SemaResult& r) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("not runtime-distinguishable") != std::string::npos)
            ++n;
    return n;
}

// A phantom `Wrap<W>` (its W is only a type argument, never stored — isolates the
// descriptor-key recursion), a concrete `Foo`, a `Box<U>` wrapper, a generic
// interface `IMarker<T>` and an implementing `C<T>`, then a `class Program` body.
std::string wrap(const std::string& body) {
    return "module M;\n"
           "class Wrap<W>\nend Wrap;\n"
           "class Foo\nend Foo;\n"
           "class Box<U>\nend Box;\n"
           "interface IMarker<T>\nend IMarker;\n"
           "sealed class C<T> : IMarker<T>, IMarker<int>\nend C;\n"
           "class Program\n" +
           body + "end Program;\n";
}

}  // namespace

// ---- REJECTED: a function-typed argument is runtime-ERASED (the headline) ------

// (1) The exact reported false positive, plain `Object` scrutinee, VALUE form.
// Without the fix `var n := case o of var x: Wrap<(int)->int> => 1, ...` reached
// the collapsed descriptor and a `Wrap<(string)->string>` value mis-bound at
// runtime (FAILS WITHOUT: the arm compiled). REJECTED at compile time.
BEZEL_TEST(case_type_test_value_form_erased_2018, is_function_arg_rejected) {
    auto r = parseSemaAll(wrap(
        "  public method M(o: Object): int\n"
        "    var n := (case o of\n"
        "      var x: Wrap<(int) -> int> => 1,\n"
        "      _ => 0\n"
        "    );\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasValueCaseErasedError(r));
}

// (2) NESTED function-typed argument: the erasure must be caught at any depth.
// `Wrap<Wrap<(int)->int>>` reifies its outer Named but its inner argument is a
// function type, so the descriptor collapses. REJECTED (pins the recursion).
BEZEL_TEST(case_type_test_value_form_erased_2018,
           nested_function_arg_rejected) {
    auto r = parseSemaAll(wrap(
        "  public method M(o: Object): int\n"
        "    var n := (case o of\n"
        "      var x: Wrap<Wrap<(int) -> int>> => 1,\n"
        "      _ => 0\n"
        "    );\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_TRUE(hasValueCaseErasedError(r));
}

// (3) A NON-generic interface scrutinee is also "plain" (no generic args to
// constrain the value), so the guard fires there too. `IThing` scrutinee, arm
// `var x: Wrap<(int)->int>`. REJECTED.
BEZEL_TEST(case_type_test_value_form_erased_2018,
           nongeneric_interface_scrutinee_function_arg_rejected) {
    auto r = parseSemaAll(
        "module M;\n"
        "interface IThing\nend IThing;\n"
        "class Wrap<W> : IThing\nend Wrap;\n"
        "class Program\n"
        "  public method M(o: IThing): int\n"
        "    var n := (case o of\n"
        "      var x: Wrap<(int) -> int> => 1,\n"
        "      _ => 0\n"
        "    );\n"
        "    return n;\n"
        "  end M;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(hasValueCaseErasedError(r));
}

// ---- ACCEPTED: reified arguments stay admitted (no over-rejection) -------------

// (4) A concrete PRIMITIVE argument is runtime-reified. `var x: Wrap<int>` — the
// common case must not be rejected.
BEZEL_TEST(case_type_test_value_form_erased_2018, primitive_arg_accepted) {
    auto r = parseSemaAll(wrap(
        "  public method M(o: Object): int\n"
        "    var n := (case o of\n"
        "      var x: Wrap<int> => 1,\n"
        "      _ => 0\n"
        "    );\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_FALSE(hasValueCaseErasedError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (5) A concrete NAMED-class argument is reified. `var x: Wrap<Foo>` — ACCEPTED.
BEZEL_TEST(case_type_test_value_form_erased_2018, named_class_arg_accepted) {
    auto r = parseSemaAll(wrap(
        "  public method M(o: Object): int\n"
        "    var n := (case o of\n"
        "      var x: Wrap<Foo> => 1,\n"
        "      _ => 0\n"
        "    );\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_FALSE(hasValueCaseErasedError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (6) A NESTED reified argument: `var x: Wrap<Wrap<int>>` — reifies recursively
// through the concrete inner primitive. ACCEPTED.
BEZEL_TEST(case_type_test_value_form_erased_2018, nested_reified_arg_accepted) {
    auto r = parseSemaAll(wrap(
        "  public method M(o: Object): int\n"
        "    var n := (case o of\n"
        "      var x: Wrap<Wrap<int>> => 1,\n"
        "      _ => 0\n"
        "    );\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_FALSE(hasValueCaseErasedError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (7) A FOLDED inline-array argument is runtime-distinguishable post-#2001 (the
// descriptor key encodes N). `var x: Wrap<[3]int>` — ACCEPTED (the guard must
// trust `typeArgRuntimeReified`, which now admits a folded inline array; the
// #2001 admission must not be undone here).
BEZEL_TEST(case_type_test_value_form_erased_2018, inline_array_arg_accepted) {
    auto r = parseSemaAll(wrap(
        "  public method M(o: Object): int\n"
        "    var n := (case o of\n"
        "      var x: Wrap<[3]int> => 1,\n"
        "      _ => 0\n"
        "    );\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_FALSE(hasValueCaseErasedError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (8) A NON-generic target `var x: Foo` (no type arguments) is untouched — the
// guard only fires on a CONSTRUCTED generic. ACCEPTED.
BEZEL_TEST(case_type_test_value_form_erased_2018, nongeneric_target_accepted) {
    auto r = parseSemaAll(wrap(
        "  public method M(o: Object): int\n"
        "    var n := (case o of\n"
        "      var x: Foo => 1,\n"
        "      _ => 0\n"
        "    );\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_FALSE(hasValueCaseErasedError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// (9) A BARE TYPE PARAMETER argument in a generic body resolves to a concrete
// argument at monomorphization, so it is NOT statically erased — the same
// conservative-accept posture the guard above takes (#1843). `var x: Wrap<T>`
// inside `Chk<T>` must stay ACCEPTED (the over-rejection guard: the guard must
// hold off a type-variable target — the design-gated #2012 residual).
BEZEL_TEST(case_type_test_value_form_erased_2018,
           bare_type_parameter_accepted) {
    auto r = parseSemaAll(wrap(
        "  public method Chk<T>(o: Object): int\n"
        "    var n := (case o of\n"
        "      var x: Wrap<T> => 1,\n"
        "      _ => 0\n"
        "    );\n"
        "    return n;\n"
        "  end Chk;\n"));
    BEZEL_EXPECT_FALSE(hasValueCaseErasedError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---- Parity with the generic-INTERFACE path: rejected there, exactly ONCE ------

// (10) A generic-INTERFACE scrutinee with an erased function-typed argument and a
// conforming sibling is STILL rejected — but by `typeTestInterfaceBindAmbiguous`
// (#1927/#1946), NOT this new guard, and reported EXACTLY once. `C<T>: IMarker<T>,
// IMarker<int>` at scrutinee `IMarker<int>`, arm `var x: C<(int)->int>`: the guard
// must SKIP the generic-interface domain (no double-report), while the interface
// predicate keeps rejecting it.
BEZEL_TEST(case_type_test_value_form_erased_2018,
           generic_interface_scrutinee_still_rejected_once) {
    auto r = parseSemaAll(wrap(
        "  public method M(m: IMarker<int>): int\n"
        "    var n := (case m of\n"
        "      var x: C<(int) -> int> => 1,\n"
        "      _ => 0\n"
        "    );\n"
        "    return n;\n"
        "  end M;\n"));
    // Rejected via the interface guard (the #1946 wording), NOT the plain-scrutinee
    // guard, and reported exactly once (the guard skipped the interface domain).
    BEZEL_EXPECT_FALSE(hasValueCaseErasedError(r));
    BEZEL_EXPECT_EQ(countErasureErrors(r), 1);
}

// (11) A generic-INTERFACE scrutinee with a REIFIED argument that
// `typeTestInterfaceBindAmbiguous` soundly ADMITS (the unique/precise #1843
// conformer) must STAY admitted — the new guard must not override its `None`.
// `C<T>: IMarker<T>, IMarker<int>` at scrutinee `IMarker<int>`, arm
// `var x: C<string>` (conforms via the ground clause). ACCEPTED, no error.
BEZEL_TEST(case_type_test_value_form_erased_2018,
           generic_interface_scrutinee_reified_admitted) {
    auto r = parseSemaAll(wrap(
        "  public method M(m: IMarker<int>): int\n"
        "    var n := (case m of\n"
        "      var x: C<string> => 7,\n"
        "      _ => 3\n"
        "    );\n"
        "    return n;\n"
        "  end M;\n"));
    BEZEL_EXPECT_FALSE(hasValueCaseErasedError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

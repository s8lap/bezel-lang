// fix #5099 (§22.6 stage 8 / §22.7 / §22.10, v0.307): at `new C(...)` on a
// local class with a primary constructor AND body constructors, the primary is
// a §22.6 candidate on equal footing with the body constructors. When no
// candidate is unambiguously better, the call is ambiguous and is a compile
// error. §22.6 says the compiler "never silently selects one".
//
// fix-forward #5132 (§11.1 / §9.3 / §22.6, v0.308) — the original rows 1, 2,
// 3 pinned the §11.1 ambiguity rule with the same-arity, same-type,
// DIFFERENT-name body ctor `class C(x: int)` + `constructor(y: int)`. Under
// the v0.308 ruling that shape is rejected at the DECLARATION (§11.1: names
// do not distinguish overloads), so the rows' `new C(...)` call sites never
// run. Per dc redo-2 NOT-PICKABLE ruling (per
// memory `feedback-reviewer-deleted-rows-reject-trigger`: deleted rows = REJECT
// trigger), rows 1, 2, and 3 are RESTORED as declaration-side REJECT rows
// asserting the new §11.1 diagnostic. The original call-site behavior they
// pinned is unobservable in a v0.308 program because the declaration is
// rejected before any `new C(...)` call is parsed. Rows 4, 5, and 6 still
// hold: different types, different arity, and the co-equal-implicit-
// conversion ambiguity between a primary `(x: long)` and a body `(y:
// double)` are all on shapes §11.1 admits, and the §22.6 ambiguity / non-
// ambiguity rules apply.
//
// Before the fix, sema deferred the whole combined shape to codegen, and
// `ctorForArgs` silently chose the body constructor. `class C(x: int)` beside
// `constructor(y: int) this(y);` made `new C(1)` print "body".
//
// Scope: classes only. The record analogue is claimed by #5100 (peer pool)
// and is not changed here. The declaration-side question (does §11.1's "same
// parameter list" include names?) is #4775 Q1 — closed in v0.308 by #5132.
//
// FAIL-WITHOUT (MEASURED, sema change reverted, rest of branch intact): all
// three restored declaration-reject rows compile clean pre-fix (the class
// shape `class C(x: int)` + `constructor(y: int)` is §11.1-admitted under
// the v0.307 names predicate), so the §11.1 diagnostic is absent pre-fix
// and present post-fix. The two accept rows pass on both sides, and the
// one reject row (row 6) compiles and runs on both sides.

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains,
                                            // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

#include <cstdio>
#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasError(const JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

void expectRejected(const JitResult& r, const char* diag) {
    if (!hasError(r, diag)) {
        std::fprintf(stderr, "  expected diagnostic containing: %s\n", diag);
        for (const auto& d : r.diagnostics.diagnostics())
            std::fprintf(stderr, "    got: %s\n", d.message.c_str());
    }
    BEZEL_EXPECT_TRUE(hasError(r, diag));
    BEZEL_EXPECT_FALSE(r.ran);
}

// Asserts the v0.308 §11.1 declaration-side clash diagnostic fires on a
// combined-form class whose body ctor differs from the primary ONLY by
// parameter names (and/or default presence). Per fix-forward #5132
// (closes #4775 Q1/Q3/Q4): "same parameter list" is §22.6 overload
// identity, so names and defaults do not exempt the body ctor from the
// §11.1 redeclaration rule.
void expectClassPrimaryRedeclared11_1(const std::string& program) {
    auto r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r,
        "body constructor redeclares the primary constructor"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""11.1"));
}

} // namespace

// Row 1 (RESTORED for redo-2, fix-forward #5132 §11.1, v0.308):
// The original row pinned the §22.6 stage-8 ambiguity rule on the same-arity,
// same-type, different-name shape `class C(x: int)` + `constructor(y: int)`.
// Under v0.308 that shape is a DECLARATION clash (§11.1 names do not
// distinguish overloads), so the `new C(1)` call site never runs. The row is
// restored as a declaration-side REJECT row asserting the new §11.1
// diagnostic on the issue's verbatim repro.
BEZEL_TEST(primary_body_ctor_ambiguity_5099,
           rejects_differs_only_by_names_class_decl_spec_11_1) {
    expectClassPrimaryRedeclared11_1(
        "module M; "
        "class C(x: int) "
        "  public constructor(y: int) "
        "    this(y + 1); "
        "    Console.WriteLine(\"body\"); "
        "  end constructor; "
        "  public method X(): int => x; "
        "end C; "
        "class P public static method Main(): int "
        "  var c := new C(1); "
        "  Console.WriteLine(\"made\"); "
        "  return 0; "
        "end Main; end P;");
}

// Row 2 (RESTORED for redo-2, fix-forward #5132 §11.1, v0.308):
// The original row pinned the §22.5 named-arg-to-primary selection rule on
// the same shape `class C(x: int)` + `constructor(y: int)`. Under v0.308 the
// declaration is rejected at §11.1, so the named-arg call site is dead
// code. The row is restored as a declaration-side REJECT row with the same
// declaration shape and a named-arg call site preserved verbatim (the named
// arg is unreachable, so the program is rejected before the arg is evaluated).
BEZEL_TEST(primary_body_ctor_ambiguity_5099,
           rejects_differs_only_by_names_named_arg_primary_decl_spec_11_1) {
    expectClassPrimaryRedeclared11_1(
        "module M; "
        "class C(x: int) "
        "  public constructor(y: int) "
        "    this(y + 1); "
        "  end constructor; "
        "  public method X(): int => x; "
        "end C; "
        "class P public static method Main(): int "
        "  var c := new C(x := 7); "
        "  Console.WriteLine(\"made\"); "
        "  return 0; "
        "end Main; end P;");
}

// Row 3 (RESTORED for redo-2, fix-forward #5132 §11.1, v0.308):
// The original row pinned the §22.5 named-arg-to-body selection rule on the
// same shape `class C(x: int)` + `constructor(y: int)`. Under v0.308 the
// declaration is rejected at §11.1, so the named-arg-to-body call site is
// dead code. The row is restored as a declaration-side REJECT row with the
// same declaration shape and a named-arg-to-body call site preserved
// verbatim. FAIL-WITHOUT (semantic note): the body constructor's `this(y + 1)`
// chain and the `Console.WriteLine("body")` side effect are unreachable in
// a v0.308 program (the declaration is rejected before the chain is
// type-checked), so the chain is verified ONLY to have parsed, not to have
// executed.
BEZEL_TEST(primary_body_ctor_ambiguity_5099,
           rejects_differs_only_by_names_named_arg_body_decl_spec_11_1) {
    expectClassPrimaryRedeclared11_1(
        "module M; "
        "class C(x: int) "
        "  public constructor(y: int) "
        "    this(y + 1); "
        "    Console.WriteLine(\"body\"); "
        "  end constructor; "
        "  public method X(): int => x; "
        "end C; "
        "class P public static method Main(): int "
        "  var c := new C(y := 7); "
        "  return 0; "
        "end Main; end P;");
}

// Row 4: a non-tying overload (`int` primary, `string` body). Each call has
// exactly one applicable candidate, so both are accepted and select correctly.
BEZEL_TEST(primary_body_ctor_ambiguity_5099,
           accepts_non_tying_types_primary_and_body_ctor_spec_22_6) {
    auto r = runAot(
        "module M; "
        "class C(x: int) "
        "  public constructor(s: string) "
        "    this(s.Length); "
        "  end constructor; "
        "  public method X(): int => x; "
        "end C; "
        "class P public static method Main(): int "
        "  var a := new C(3); "
        "  var b := new C(\"four\"); "
        "  return a.X() * 10 + b.X(); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 34);
}

// Row 5: a body constructor of a different arity (the #2362 Widget shape).
// §22.5 arity filtering leaves one candidate per call.
BEZEL_TEST(primary_body_ctor_ambiguity_5099,
           accepts_different_arity_primary_and_body_ctor_spec_22_5) {
    auto r = runAot(
        "module M; "
        "class Widget(id: int) "
        "  public constructor(id: int, note: string) "
        "    this(id); "
        "  end constructor; "
        "  public method Id(): int => id; "
        "end Widget; "
        "class P public static method Main(): int "
        "  var a := new Widget(5); "
        "  var b := new Widget(6, \"n\"); "
        "  return a.Id() * 10 + b.Id(); "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 56);
}

// Row 6: both candidates need an implicit conversion (`int` to `long` and
// `int` to `double`). §22.6 ranks all implicit conversions as co-equal, so
// the call is ambiguous. The primary is ranked by the same rules as a body
// ctor.
BEZEL_TEST(primary_body_ctor_ambiguity_5099,
           rejects_co_equal_conversions_primary_vs_body_ctor_spec_22_6) {
    auto r = runAot(
        "module M; "
        "class C(x: long) "
        "  public constructor(y: double) "
        "    this(1); "
        "  end constructor; "
        "end C; "
        "class P public static method Main(): int "
        "  var c := new C(3); "
        "  return 0; "
        "end Main; end P;");
    expectRejected(r, "ambiguous call to `new C`: no overload is unambiguously "
                      "better (§22.6)");
}

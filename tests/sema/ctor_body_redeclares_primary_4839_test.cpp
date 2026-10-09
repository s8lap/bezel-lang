// §11.1 (issue #4839, v0.307): a class body may declare a constructor, but
// it must NOT have the same parameter list as the primary constructor. The spec
// (v0.307) §11.1 states: "The body may not redeclare a constructor with the
// same parameter list as the primary constructor."
//
// fix-forward #5132 (§11.1 / §9.3 / §22.6, v0.308): "same parameter list" is
// §11.3's clash sense, i.e. §22.6 overload identity — arity, parameter
// types, `ref`/`out` modifiers, ordinary `owned`. Parameter NAMES and
// DEFAULTS do not distinguish. So body ctors differing from the primary
// only by name (rows 4 and 7) or only by case (row 6, the §9.8 combined-
// form record Vec2) are now REJECTED. Rows 4, 6, and 7 are flipped in
// this commit to the §11.1 / §9.3 reject oracle.
//
// WHAT THE DEFECT WAS. The existing §11.3/§22.6 overload clash check compares
// body constructors against each other (types-only), and the primary constructor
// is not included in that group (it carries no ConstructorSymbol). Nothing
// compared a body constructor against the primary's parameter list, so a body
// ctor with the same parameter list compiled and ran.
//
// WHAT THE FIX DOES (as narrowed by the fix-forward). In
// `Resolver::checkTypeConstructorChains`, for a CLASS with a primary
// constructor, a body constructor whose list matches the primary's at every
// position — same name, same resolved type, same §22.6 overload modifier,
// same default presence — gets the §11.1 error. Two things are deliberately
// left alone because issue #4775 (Q1/Q2) has not been ruled:
//   * same types with DIFFERENT names (does "same parameter list" include
//     names?) — rows 4 and 7;
//   * RECORDS, whose primary is governed by §9.3's own sentence and whose
//     §9.8 step 2 requires the name-matching explicit constructor that a
//     types-only §9.3 reading would forbid — row 6.
//
// REJECT rows (v0.308): same names + same types (rows 1, 5), same types
// different names (row 4), the §9.8 record explicit-ctor match (row 6),
// the `owned` variant same name (row 8), the `owned` variant different
// name #3751 shape (row 7). ACCEPT rows: different types (row 2), different
// arity (row 3).

#include <cstdio>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// Error-severity only: the §11.1 / §9.3 diagnostic is an Error, so a reject
// row must see it AS an Error, and an accept row's EXPECT_FALSE is then the
// (redundant, not vacuous) Error-only form - the real accept oracle is the
// program's exit code and stdout (check_vacuous_tests, #3138).
bool hasError(const JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(sub) != std::string::npos)
            return true;
    return false;
}

void dumpDiagnostics(const JitResult& r) {
    std::fprintf(stderr, "  diagnostics (count=%zu):\n",
                 r.diagnostics.diagnostics().size());
    for (const auto& d : r.diagnostics.diagnostics())
        std::fprintf(stderr, "    [%s]\n", d.message.c_str());
}

}  // namespace

// ---------------------------------------------------------------------------
// Row 1 — REJECT: the issue's repro. `class K(a: int, b: int)` with body
// `constructor(a: int, b: int) this(a, b);` — same parameter types, same
// count — is rejected by §11.1. Sema must emit the redeclares-primary
// diagnostic.
// ---------------------------------------------------------------------------
BEZEL_TEST(ctor_body_redeclares_primary_4839,
           rejects_same_param_list_as_primary_spec_11_1) {
    auto r = runAot(
        "module M; "
        "class K(a: int, b: int) "
        "  public constructor(a: int, b: int) "
        "    this(a, b); "
        "  end constructor; "
        "end K; "
        "class C "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (!hasError(r, "redeclares the primary constructor") ||
        !hasError(r, "§11.1")) {
        dumpDiagnostics(r);
    }
    BEZEL_EXPECT_TRUE(hasError(r, "redeclares the primary constructor"));
    BEZEL_EXPECT_TRUE(hasError(r, "§11.1"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---------------------------------------------------------------------------
// Row 2 — ACCEPT: body ctor with DIFFERENT parameter TYPES. The primary is
// `(a: int, b: int)` but the body ctor takes `(x: string, y: string)` —
// different types at each position. This is not a redeclaration and must
// compile (though it may fail the convenience-ctor chain rule — that's a
// separate issue from §11.1 redeclaration).
// ---------------------------------------------------------------------------
BEZEL_TEST(ctor_body_redeclares_primary_4839,
           accepts_different_param_types_from_primary) {
    auto r = runAot(
        "module M; "
        "class K(a: int, b: int) "
        "  public constructor(x: string, y: string) "
        "  end constructor; "
        "end K; "
        "class C "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    // Accept regardless of whether the convenience-ctor chain rule fires;
    // this row only verifies that the §11.1 redeclaration check is not
    // triggered by a type mismatch.
    BEZEL_EXPECT_TRUE(r.ran || !hasError(r, "redeclares the primary constructor"));
}

// ---------------------------------------------------------------------------
// Row 3 — ACCEPT control: primary `(a: int, s: string)` and a convenience
// body ctor `(n: int)` that chains to the primary. Different arity — not a
// redeclaration. Must compile and run.
// ---------------------------------------------------------------------------
BEZEL_TEST(ctor_body_redeclares_primary_4839,
           accepts_convenience_ctor_different_arity) {
    auto r = runAot(
        "module M; "
        "class K(a: int, s: string) "
        "  private _a: int := a; "
        "  private _s: string := s; "
        "  public constructor(n: int) "
        "    this(n, \"default\"); "
        "  end constructor; "
        "  public method A(): int return _a; end A; "
        "  public method S(): string return _s; end S; "
        "end K; "
        "class C "
        "  public static method Main(): int "
        "    var k := new K(42); "
        "    Console.WriteLine(k.A().ToString()); "
        "    Console.WriteLine(k.S()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (r.diagnostics.hasErrors()) dumpDiagnostics(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("42\ndefault\n"));
}

// ---------------------------------------------------------------------------
// Row 4 — FLIPPED for fix #5132 (§11.1 / §22.6, v0.308). Single-parameter
// primary `(x: int)` and a body ctor `(y: int)` — same type, same modifier,
// DIFFERENT name — was an ACCEPT control under the v0.307 narrow predicate
// (which compared names); under v0.308 §22.6 overload identity the parameter
// lists are identical (names do not distinguish), so §11.1 REJECTS. Same row,
// new expectation.
// ---------------------------------------------------------------------------
BEZEL_TEST(ctor_body_redeclares_primary_4839,
           rejects_same_types_different_names_spec_11_1) {
    auto r = runAot(
        "module M; "
        "class K(x: int) "
        "  public constructor(y: int) "
        "    this(y + 1); "
        "  end constructor; "
        "end K; "
        "class C "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (!hasError(r, "redeclares the primary constructor")) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, "redeclares the primary constructor"));
    BEZEL_EXPECT_TRUE(hasError(r, "§11.1"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---------------------------------------------------------------------------
// Row 5 — REJECT: single-param primary `(a: int)` and body ctor `(a: int)`
// same types and same names — redeclaration, rejected by §11.1.
// ---------------------------------------------------------------------------
BEZEL_TEST(ctor_body_redeclares_primary_4839,
           rejects_single_param_same_type) {
    auto r = runAot(
        "module M; "
        "class K(a: int) "
        "  public constructor(a: int) "
        "    this(a); "
        "  end constructor; "
        "end K; "
        "class C "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    if (!hasError(r, "redeclares the primary constructor") ||
        !hasError(r, "§11.1")) {
        dumpDiagnostics(r);
    }
    BEZEL_EXPECT_TRUE(hasError(r, "redeclares the primary constructor"));
    BEZEL_EXPECT_TRUE(hasError(r, "§11.1"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---------------------------------------------------------------------------
// Row 6 — FLIPPED for fix #5132 (§9.3 / §22.6, v0.308). `record Vec2(X, Y)`
// with `constructor(x, y)` was an ACCEPT (fix-forward) under the v0.307
// predicate (case-sensitive name comparison): the body ctor's parameter
// list differed from the primary only by case. Under §22.6 overload
// identity (names do not distinguish), the lists are identical and §9.3
// REJECTS the declaration. Same shape, new expectation.
// ---------------------------------------------------------------------------
BEZEL_TEST(ctor_body_redeclares_primary_4839,
           rejects_record_combined_form_explicit_ctor_match_spec_9_3) {
    auto r = runAot(
        "module M; "
        "record Vec2(X: int, Y: int): ISerializable, IDeserializable "
        "  public constructor(x: int, y: int) "
        "    X := x + 100; "
        "    Y := y + 100; "
        "  end constructor; "
        "end Vec2; "
        "class C "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (!hasError(r, "redeclares the primary constructor")) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, "redeclares the primary constructor"));
    BEZEL_EXPECT_TRUE(hasError(r, "\xC2\xA7""9.3"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---------------------------------------------------------------------------
// Row 7 — FLIPPED for fix #5132 (§11.1 / §22.6, v0.308). The #3751 shape
// (`class Base(owned r: Res)` with `constructor(owned x: Res) this(owned x);`)
// was an ACCEPT under the v0.307 predicate (names differ, but the predicate
// included names). Under §22.6 overload identity, ordinary `owned` is part
// of the matching key and names do not distinguish, so the lists are
// identical and §11.1 REJECTS. (Lit row
// this_chain_3751__owned_marker_to_primary_ctor_accepted was flipped in the
// same commit.)
// ---------------------------------------------------------------------------
BEZEL_TEST(ctor_body_redeclares_primary_4839,
           rejects_owned_convenience_ctor_different_name_3751_shape) {
    auto r = runAot(
        "module M; "
        "class Res "
        "  private _id: int; "
        "  public constructor(id: int) _id := id; end constructor; "
        "  public method Id(): int => _id; "
        "end Res; "
        "class Base(owned r: Res) "
        "  private owned _s: Res := r; "
        "  public constructor(owned x: Res) "
        "    this(move x); "
        "  end constructor; "
        "  public method Id(): int return _s.Id(); end Id; "
        "end Base; "
        "class C "
        "  public static method Main(): int return 0; "
        "  end Main; "
        "end C;");
    if (!hasError(r, "redeclares the primary constructor")) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, "redeclares the primary constructor"));
    BEZEL_EXPECT_TRUE(hasError(r, "§11.1"));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---------------------------------------------------------------------------
// Row 8 — REJECT: the `owned` variant with the SAME name. `class
// Base(owned r: Res)` with `constructor(owned r: Res)` matches the primary in
// name, type and §22.6 modifier — rejected under every reading of §11.1.
// ---------------------------------------------------------------------------
BEZEL_TEST(ctor_body_redeclares_primary_4839,
           rejects_owned_same_name_same_type) {
    auto r = runAot(
        "module M; "
        "class Res "
        "  private _id: int; "
        "  public constructor(id: int) _id := id; end constructor; "
        "end Res; "
        "class Base(owned r: Res) "
        "  private owned _s: Res := r; "
        "  public constructor(owned r: Res) "
        "    this(move r); "
        "  end constructor; "
        "end Base; "
        "class C "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    if (!hasError(r, "redeclares the primary constructor")) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(hasError(r, "redeclares the primary constructor"));
    BEZEL_EXPECT_TRUE(hasError(r, "§11.1"));
    BEZEL_EXPECT_FALSE(r.ran);
}

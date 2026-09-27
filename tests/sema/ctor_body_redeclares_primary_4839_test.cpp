// §11.1 (issue #4839, v0.307): a class body may declare a constructor, but
// it must NOT have the same parameter list as the primary constructor. The spec
// (v0.307) §11.1 states: "The body may not redeclare a constructor with the
// same parameter list as the primary constructor."
//
// WHAT THE DEFECT WAS. The existing §11.3/§22.6 overload clash check compares
// body constructors against each other (types-only), and the primary constructor
// is not included in that group (it carries no ConstructorSymbol). Nothing
// compared a body constructor against the primary's parameter list, so a body
// ctor with the same type sequence compiled and ran — creating an ambiguous
// dispatch situation.
//
// WHAT THE FIX DOES. In `Resolver::checkTypeConstructorChains`, after the
// existing chain checks, add a check for types with a primary constructor: for
// each body constructor, compare its parameter types against the primary's
// positionalFields types. If all match (count, types, and modifiers), emit a
// §11.1 error: "body constructor redeclares the primary constructor: it has
// the same parameter list."
//
// REJECT rows: the issue's repro — primary `class K(a: int, b: int)` and body
// `constructor(a: int, b: int) this(a, b);` — must produce the §11.1
// diagnostic. ACCEPT rows: body ctor with DIFFERENT parameter list — different
// types, different arity, or different names — must compile and run cleanly.

#include <cstdio>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

bool hasError(const JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
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
        "  constructor(a: int, b: int) "
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
// Row 4 — ACCEPT control: single-parameter primary `(x: int)` and a
// convenience body ctor `(y: int)` that chains to the primary. Same type,
// different name — not a redeclaration under types-only reading.
// ---------------------------------------------------------------------------
BEZEL_TEST(ctor_body_redeclares_primary_4839,
           accepts_same_types_different_names) {
    auto r = runAot(
        "module M; "
        "class K(x: int) "
        "  public constructor(y: int) "
        "    this(y + 1); "
        "  end constructor; "
        "  public method V(): int return x; end V; "
        "end K; "
        "class C "
        "  public static method Main(): int "
        "    var k := new K(7); "
        "    Console.WriteLine(k.V().ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (r.diagnostics.hasErrors()) dumpDiagnostics(r);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("8\n"));
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
        "  constructor(a: int) "
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

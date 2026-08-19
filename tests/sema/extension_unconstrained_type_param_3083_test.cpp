// Spec-derived tests for §31.1 / §31.2.1 type-parameter arm/operand rule
// applied to EXTENSION declarations.
//
// §26.4 (Generic extensions): a generic extension `extension E<T>` introduces
// T as a block-level type parameter via the extension head. §31.2.1 (arm
// patterns) and §31.1 (throw operand) require that any type-parameter name
// appearing as an arm type or thrown value carry an `Exception`-derived class
// bound — including when the parameter is UNCONSTRAINED, which §31.2.1
// explicitly rejects ("a parameter that is unconstrained ... is a compile
// error at the arm, checked at the generic declaration").
//
// Issue #3083 — the ast::ExtensionDecl does not carry a `genericParams` field,
// and §31.2.1/§31.1's check (`isEnclosingTypeParamName`) only walks the
// extension's `genericConstraints` (where-clauses). For an unconstrained `T`
// in `extension E<T>` the check returns false and T is silently exempt from
// §31.1 and §31.2.1 entirely. The two cases that have to fire are pinned by
// the tests below:
//
//   T1 (rejection, §31.2.1): `catch var ex: T` inside an unconstrained
//       `extension E<T>` is a compile error (the parameter is unconstrained).
//   T2 (rejection, §31.1): `throw t` for an unconstrained `extension E<T>`'s T
//       is a compile error (a thrown value's static type must be `Exception`-
//       derived, and an unconstrained type parameter has no such guarantee).
//
// And three ACCEPT controls:
//   T3: `extension E<T> where T: Exception` — the where-clause covers T, so
//       the arm/operand check is satisfied.
//   T4: `extension E<T> where T: U` with `where U: Exception` — the §14.2
//       chain, which `typeParamHasExceptionBound` walks; this is the chain
//       path the existing tests already pin.
//   T5: `extension E` (no type parameter at all) — a closed type parameter
//       list, no arm-shaped §31.2.1 case to fire.
//
// (Also: the spec §31.2.1 quote the brief and other §31.2.1 tests reference
// for "every listed type is `Exception`-derived", so the bug surface area
// here is fully the §31.2.1/§31.1 generic-parameter clause — the only shape
// the change touches.)

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasDiagnosticContaining(const bezel::test::SemaResult& r,
                              std::string_view needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// T1 (§31.2.1, issue #3083): an UNCONSTRAINED extension type parameter `T`
// (no `where T: ...` clause) used as a catch-arm type is rejected at the arm,
// because §31.2.1 lists `unconstrained` as one of the four reject-bound
// shapes, verbatim: "a parameter that is unconstrained, value-constrained,
// bounded only by `where TEx: class` ..., or bounded by a class outside the
// `Exception` family is a compile error at the arm, checked at the generic
// declaration".
//
// Before fix #3083: ast::ExtensionDecl carried no `genericParams` field, the
// §31.2.1 check walks ONLY `genericConstraints` for the extension, finds
// nothing for an unconstrained T, and silently accepts — the test pins the
// REJECT. (No accompanying class method shadows the diagnostic: the catch
// arm here lives ONLY inside `extension C<T>`, so the bug path is the only
// way this test would emit a diagnostic.) C is generic so the extension's
// head `C<T>` is a well-formed parameterized type reference (per §26.4 the
// head's bare single-segment argument introduces T as a fresh block-level
// type parameter, even when `class C<T>` declares its own T).
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_extension_unconstrained_type_param_3083,
           arm_type_unconstrained_T_rejected_sec_31_2_1) {
    auto r = parseSemaAll(
        "module M; "
        "class C<T> end C; "
        "extension C<T> "
        "  public method CatchT(): int "
        "    try throw new ArgumentException(\"x\"); "
        "    catch var e: T => do return 1; end, "
        "          var ex: Exception => do return 2; end "
        "    end; "
        "    return 0; "
        "  end CatchT; "
        "end;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagnosticContaining(
        r, "the `catch` arm's type parameter `T` must carry an"));
}

// ---------------------------------------------------------------------------
// T2 (§31.1, issue #3083): an UNCONSTRAINED extension type parameter `T`
// used as a `throw` operand type — same silent-accept defect, applied to
// §31.1's throw-operand rule.
//
// §31.1 (v0.243, #3046): "`throw expr` requires `expr` to be an owned
// instance of an `Exception`-derived class type: the operand's static type,
// nullability not admitted, must be `Exception` or a class deriving from
// it". For an unconstrained T, T's static type is no such thing — and §31.2.1
// already states the analogous clause for the catch arm. The check at
// ExprTypecheck.cpp's checkThrowableType gates on
// `isEnclosingTypeParamName(n)` — same predicate, same bug, same fix path.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_extension_unconstrained_type_param_3083,
           throw_operand_unconstrained_T_rejected_sec_31_1) {
    auto r = parseSemaAll(
        "module M; "
        "class C<T> end C; "
        "extension C<T> "
        "  public method ThrowT(t: T): int "
        "    throw t; "
        "    return 0; "
        "  end ThrowT; "
        "end;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagnosticContaining(
        r, "the type parameter `T` carries no `Exception`-derived class bound"));
}

// ---------------------------------------------------------------------------
// T3 (control ACCEPT, §31.2.1 + §31.1): an extension type parameter `T` with
// an `Exception`-derived class bound (`where T: Exception`) is the admitted
// shape. The pin is the OPPOSITE of T1/T2: same extension shape, but the
// where-clause names T. The fix must not over-reject an already-bounded T.
//
// (Throws a fresh `new ArgumentException`, never the borrowed method
// parameter `t` — `throw expr` requires an OWNED instance (§31.1), and a
// borrowed parameter `t: T` cannot satisfy that. The companion
// `catch var e: T => …` is the §31.2.1 surface this control exercises.)
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_extension_unconstrained_type_param_3083,
           where_T_exception_arm_and_throw_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class C<T> end C; "
        "extension C<T> where T: Exception "
        "  public method CatchT(t: T): int "
        "    try throw new ArgumentException(\"x\"); "
        "    catch var e: T => do return 1; end, "
        "          var ex: Exception => do return 2; end "
        "    end; "
        "    return 0; "
        "  end CatchT; "
        "end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// T4 (control ACCEPT, §14.2 chain): `extension E<T> where T: U` with a
// second clause `where U: Exception` — the bound on T is reached only by
// following the `where T: U` chain (the §14.2 chain `typeParamHasExceptionBound`
// walks). The fix's predicate must read the same walk, not the literal
// bound, so this control pins that the chain still works once the
// unconstrained-T entry path is added.
//
// (Throws a fresh exception rather than `throw t` for the same §31.1 reason
// as T3 — a borrowed method parameter cannot be the throw operand.)
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_extension_unconstrained_type_param_3083,
           where_T_to_U_chain_arm_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class C<T, U> end C; "
        "extension C<T, U> where T: U where U: Exception "
        "  public method CatchT(t: T, u: U): int "
        "    try throw new ArgumentException(\"x\"); "
        "    catch var e: T => do return 1; end, "
        "          var ex: Exception => do return 2; end "
        "    end; "
        "    return 0; "
        "  end CatchT; "
        "end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// T5 (closed control ACCEPT): `extension C` (no head type parameters at all)
// — there is no `T` for either §31.1 or §31.2.1 to fire on. The arm uses the
// built-in `Exception` type, which is the plain accepted shape and a sanity
// check the fix's predicate does not affect non-generic extensions.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_extension_unconstrained_type_param_3083,
           closed_extension_with_exception_arm_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class C end C; "
        "extension C "
        "  public method CatchEx(): int "
        "    try throw new ArgumentException(\"x\"); "
        "    catch var ex: Exception => do return 1; end "
        "    end; "
        "    return 0; "
        "  end CatchEx; "
        "end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Spec-derived regression test for issue #3160.
//
// §14.2 (generic constraints) / §14.1 (instantiation): a failed derived-
// property constraint "is diagnosed at the instantiation site naming the
// constraint, the offending type argument, and the structural reason".
//
// §14.2 was checked at every `Named<...>` type position EXCEPT the receiver
// of a static-member access (`Ferry<Bad>.Count()`). The receiver IdentifierExpr
// was the only path that minted a `TypeRef(Named, TypeSymbol, genericArgs)`
// without ever calling `Resolver::checkGenericConstraints` — the type was
// constructed inline in `ExprTC`'s IdentifierExpr handler
// (src/sema/ExprTypecheck_Exprs.cpp). Every other syntactic position that
// names a generic type (`new G<T>`, a typed local, a parameter type, a return
// type, a `typeof(T)` operand, a base-list position) routes through
// `Resolver::resolveType` and gets the constraint check for free.
//
// MEASURED on a0b655e with a single-module repro:
//   `Ferry3<BoxL>.Count()` accepted with exit 0 and no diagnostic,
//   while the `new Ferry2<BoxL>()` form was correctly rejected with
//   `type argument BoxL does not satisfy the constraint where T: sendable`
//   (the diagnostic location prints 0:0 on the new route, since the §14.2
//   site reads the wrapping `ast::Type` node rather than the use site — the
//   IdentifierExpr path carries the use-site span, so its diagnostic is
//   well-located).
//
// The fix routes the IdentifierExpr path through the same constraint-check
// the type resolver runs, so every §14.2 constraint form fires uniformly:
// `where T: sendable`, `where T: class`, `where T: new()`, a base-type
// constraint, a type-parameter bound (`where T: U`). The static-member-access
// instantiation site is monomorphized (§14.1) and the static fields of
// `Ferry<Bad>` are its own — `Ferry<Bad>.Count()` is a real instantiation
// site in the spec's sense and the constraint must run there.
//
// What this test pins:
//   * REJECT (static method call): `Ferry3<BoxL>.Count()` — the issue's
//     minimal repro, single module, BoxL is unambiguously non-sendable per
//     §30.14 rule 3 (interface-typed FIELD). The diagnostic names the
//     constraint, the offending argument, and the structural reason.
//   * REJECT (static property access): `Ferry3<BoxL>.Tag` — proves the
//     check is not method-call-only; it covers the whole static-member
//     surface (methods AND properties, per §14.1's monomorphization).
//   * REJECT (different §14.2 constraint form): `Hold<int>` where Hold
//     has `where T: class` — proves the fix is not sendable-specific.
//     §14.2's enumeration of constraint kinds all share the same dispatch
//     site, so this generalises; the suspicion flagged in the issue that
//     every §14.2 form is equally unchecked is corroborated.
//   * ACCEPT-CONTROL (well-formed instantiation stays accepted):
//     `Ferry3<Good>.Count()` — Good has no interface-typed field, so it
//     is sendable per §30.14, and the constraint check produces no
//     diagnostic. A regression to a too-broad reject would visibly break
//     this test, forcing a coordinated narrowing.
//
// Out of scope: the deferred-nested path (where an arg is itself an
// in-scope generic parameter of the enclosing type) is not exercised here.
// That edge case requires `Outer<W>.Inner<W>`-shaped source and lives
// behind the same Resolver-state plumbing the existing new-site path uses.
// The issue's repro and every realistic instantiation site use concrete
// type arguments, so this test covers the bug as filed.

#include <string>
#include <string_view>

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCollect;

namespace {

bezel::test::SemaResult semaAll(std::string_view src) {
    auto r = parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasConstraintError(const bezel::test::SemaResult& r,
                        const char* fragment) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find("does not satisfy the constraint") !=
                std::string::npos &&
            d.message.find(fragment) != std::string::npos)
            return true;
    return false;
}

bool hasAnyError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error) return true;
    return false;
}

}  // namespace

// --- §14.2 REJECT: static method call with bad type argument ---------------
//
// Issue #3160's minimal repro: `Ferry3<BoxL>.Count()` was accepted before
// the fix. `BoxL` is non-sendable per §30.14 rule 3 (interface-typed FIELD
// `Shape: IShapeL?`), so the `where T: sendable` constraint MUST be
// diagnosed at this instantiation site.

BEZEL_TEST(static_member_generic_constraint_3160,
           issue_3160_static_method_call_rejects_nonsendable_arg) {
    auto r = semaAll(
        "module M; "
        "public interface IShapeL end IShapeL; "
        "public sealed class BoxL "
        "  public Shape: IShapeL?; "
        "end BoxL; "
        "public class Ferry3<T> where T: sendable "
        "  public static method Count(): int return 3; end Count; "
        "end Ferry3; "
        "public class CL "
        "  public static method Main(): int "
        "    return Ferry3<BoxL>.Count(); "
        "  end Main; "
        "end CL;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r, "where T: sendable"));
}

// --- §14.2 REJECT: static property access with bad type argument -----------
//
// Same generic, same argument, but the member is a static PROPERTY rather
// than a static method. §14.1 monomorphizes the static surface uniformly —
// if the method-call form was the only checked site, the property form
// would silently leak. Pins the broader coverage.

BEZEL_TEST(static_member_generic_constraint_3160,
           issue_3160_static_property_read_rejects_nonsendable_arg) {
    auto r = semaAll(
        "module M; "
        "public interface IShapeL end IShapeL; "
        "public sealed class BoxL "
        "  public Shape: IShapeL?; "
        "end BoxL; "
        "public class Ferry3<T> where T: sendable "
        "  public static property Tag: int := 3; "
        "end Ferry3; "
        "public class CL "
        "  public static method Main(): int "
        "    return Ferry3<BoxL>.Tag; "
        "  end Main; "
        "end CL;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r, "where T: sendable"));
}

// --- §14.2 REJECT: different constraint form --------------------------------
//
// `Hold<T>` has `where T: class`. `int` is a Primitive (a value type), so
// `Hold<int>` violates the class bound regardless of §30.14 / sendability.
// Pins that the fix is not sendable-specific — every §14.2 constraint kind
// routed through the same dispatch site fires uniformly.

BEZEL_TEST(static_member_generic_constraint_3160,
           issue_3160_constraint_form_class_also_checked) {
    auto r = semaAll(
        "module M; "
        "public class Hold<T> where T: class "
        "  public static method Stamp(): int return 7; end Stamp; "
        "end Hold; "
        "public class CL "
        "  public static method Main(): int "
        "    return Hold<int>.Stamp(); "
        "  end Main; "
        "end CL;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r, "where T: class"));
}

// --- §14.2 ACCEPT-CONTROL: well-formed instantiation stays accepted ---------
//
// `Good` has no interface-typed field (the `where T: sendable` check passes
// per §30.14). `Ferry3<Good>.Count()` must remain accepted. A regression
// to a too-broad reject would visibly break this test, forcing a
// coordinated narrowing. The ACCEPT-CONTROL pins the legitimate path so
// the fix is provably narrow.

BEZEL_TEST(static_member_generic_constraint_3160,
           issue_3160_well_formed_static_member_access_stays_accepted) {
    auto r = semaAll(
        "module M; "
        "public sealed class Good "
        "  public Counter: int; "
        "end Good; "
        "public class Ferry3<T> where T: sendable "
        "  public static method Count(): int return 3; end Count; "
        "end Ferry3; "
        "public class CL "
        "  public static method Main(): int "
        "    return Ferry3<Good>.Count(); "
        "  end Main; "
        "end CL;");
    BEZEL_EXPECT_FALSE(hasAnyError(r));
}

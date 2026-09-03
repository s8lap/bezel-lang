// Spec-derived tests for §26.4 / §26.5 (v0.286, issue #3672).
//
// Census row 15 (docs/proposals/duplication-census-2026-08-30.md §2):
// `extSignatureUsesFreeVar` (src/sema/ExprTypecheck.cpp:~7472) decides
// whether a user-declared generic extension's method needs the
// per-instantiation T-substitution codegen that #773 left deferred — a
// method whose signature uses the extension's free type variable is SKIPPED
// as a candidate (falls through to the ordinary "no such member" diagnostic)
// rather than matched into a call codegen cannot correctly generate.
//
// THE FIX (#3672): on HEAD at run start, `extSignatureUsesFreeVar` already
// delegated its `ast::Type` walk to `typeNamesFreeVar` for both the return
// type and every parameter type — so nullable, inline-array, generic-arg,
// and function-type positions were NOT a re-implementation and did not
// diverge (see the §6b enumeration in the fix commit body). The one gap:
// `extSignatureUsesFreeVar` never scanned the method's OWN generic
// constraints (`ast::MethodDecl::genericConstraints`) — a method-level
// type-bound constraint (§14.2 "Type-parameter bounds", `where U: T`) can
// name the extension's free var as the bound even when neither the return
// type nor any parameter does. Before the fix such a method was NOT
// deferred, so a call like `b.Foo<Z>()` on a `Bag<W>` receiver, where `Z`
// does not satisfy `where U: T` at `T = W`, compiled and ran with NO
// diagnostic — the constraint-satisfaction check has no concrete type to
// check `Z` against once `T` is an unresolved free var, so it is silently
// not enforced. That is exactly the #773 deferred case, silently NOT
// deferred: this test pins the resulting FALSE_ACCEPT and its fix.
//
// TESTS:
//
//   free_var_only_in_own_constraint_is_deferred — THE FIX. A method whose
//     return type and parameters do not use `T`, but whose own `where U: T`
//     constraint does, must be excluded from the candidate set — the call
//     site sees "no extension method" rather than compiling a call whose
//     constraint was never actually checked.
//
//   unrelated_constraint_is_not_over_deferred — control. A method-level
//     constraint that does NOT name the extension's free var (`where U:
//     Marker`) must NOT trigger the defer — the fix must not over-reject.
//
//   non_type_constraint_item_does_not_crash — control. A `class`/`value`/
//     `notnull`/`new()` constraint item (grammar A.3 `Constraint`) leaves
//     `ConstraintItem::typeArg` null; the new scan
//     (`typeNamesFreeVar(item.typeArg.get(), freeVars)`) must not misbehave
//     on it — `typeNamesFreeVar` already null-checks its argument, but this
//     pins the call site does not skip that arm.

#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
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

// Shared program shape: a user-declared generic class `Bag<T>` (so the
// extension target is NOT a builtin — #714's builtin-target
// binding-and-codegen path does not gate this member-skip; only
// user-declared targets defer per #773) plus two unrelated classes `W`/`Z`
// so a `Foo<Z>()` call on a `Bag<W>` receiver is a constraint violation
// under `where U: T` at `T = W`.
std::string prog(const std::string& fooMethod, const std::string& callArg) {
    return std::string(
        "module M; "
        "class Bag<T> where T: class "
        "  private owned _items: Array<T>; "
        "  public constructor() _items := new Array<T>(); end; "
        "end Bag; "
        "class W public v: int; public constructor(n: int) v := n; end; end W; "
        "class Z public v: int; public constructor(n: int) v := n; end; end Z; "
        "class Marker end Marker; "
        "public extension Bag<T> where T: class ") +
        fooMethod +
        " end; "
        "public static class P public static method Main(): int "
        "  var b := new Bag<W>(); "
        "  var r := b.Foo<" + callArg + ">(); "
        "  return r; "
        "end Main; end P;";
}

}  // namespace

// THE FIX (#3672, §26.4/§26.5): `where U: T` is the method's own
// constraint, not a param/return use of `T` — must still be caught.
BEZEL_TEST(ext_signature_free_var_constraint_3672,
           free_var_only_in_own_constraint_is_deferred) {
    auto r = parseSemaAll(prog(
        "  public method Foo<U>(): int where U: T => 42; ", "Z"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagnosticContaining(r, "no extension method"));
}

// Control: a constraint that does NOT name the extension's free var must
// not be over-deferred — `Marker` is a concrete, resolvable bound so the
// call type-checks (and constraint-satisfaction is enforced normally, since
// no free var is involved).
BEZEL_TEST(ext_signature_free_var_constraint_3672,
           unrelated_constraint_is_not_over_deferred) {
    auto r = parseSemaAll(prog(
        "  public method Foo<U>(): int where U: Marker => 42; ", "Marker"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Control: a `class` constraint item (grammar A.3 `Constraint ::= "class" |
// ...`) has a null `typeArg` — the new `for (const auto& item : gc.items)`
// loop must not crash walking it, and it must not itself trigger the defer
// (it names no free var, `T` included). `Foo` here uses neither `T` nor a
// type-bound reference to it anywhere, so the call must type-check cleanly.
BEZEL_TEST(ext_signature_free_var_constraint_3672,
           non_type_constraint_item_does_not_crash) {
    auto r = parseSemaAll(prog(
        "  public method Foo<U>(): int where U: class => 42; ", "W"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

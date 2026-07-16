// Spec-derived regression tests for §14.2 "Interface conformance" clause 5
// (v0.196) — issue #1867: a PRIMITIVE type argument conforms to the four
// synthesized-declaration *instance* builtin interfaces ONLY as its standard-
// library record declaration lists them, AND with the generic argument being
// the primitive itself (stdlib §9):
//   • IComparable<P>  — every primitive EXCEPT `bool` (stdlib §9.4 `Boolean`
//     lists `IEquatable<bool>` only, no `IComparable`).
//   • IEquatable<P>   — every primitive (stdlib §9.1–§9.6).
//   • IEqualityComparer<…> / IReadOnlySpan<…> — NO primitive conforms: an
//     equality-comparer *agent* (stdlib §2.18) and a span view (stdlib §4.16).
// Additionally, per §14.2 clause 5 + §28.2/§9.7, a primitive can NEVER conform
// to `IDisposable`/`IAsyncDisposable` (only classes implement them, implicitly).
//
// The defect this pins (follow-up to commit 91ae258, which closed the primitive-
// vs-USER-interface hole): the primitive-conformance predicate blanket-accepted a
// synthesized-builtin bound by interface NAME/decl without checking the generic
// argument or exact conformance. So the following were SILENTLY ACCEPTED at both
// constraint-check sites — the method-call site (ExprTypecheck_Calls.cpp) and the
// type-instantiation site (Resolver::checkOneConstraint in TypeResolution_Checks.
// cpp):
//   • `int`  vs `IComparable<long>`       — WRONG generic argument
//   • `int`  vs `IEqualityComparer<int>`  — a primitive is not an equality comparer
//   • `int`  vs `IReadOnlySpan<int>`      — a primitive is not a span
//   • `int`  vs `IDisposable`             — name-recognized, declaration-null defer
//   • `bool` vs `IComparable<bool>`       — `bool` declares no `IComparable`
// Fix: verify the bound's generic argument matches the primitive's declared
// conformance instantiation (primitiveConformsToBuiltinInstanceIface), and reject
// the declaration-null `IDisposable`/`IAsyncDisposable` bounds a primitive
// provably cannot implement (primitiveNeverConformsToBuiltinIface). The Defer arm
// keeps legitimate `IComparable<int>`/`IEquatable<int>`/`IComparable<T>` (and the
// PascalCase `IComparable<Int32>`) conformances accepted — no over-rejection.
//
// Authority: spec §14.2 clause 5 (v0.196), stdlib §9.1–§9.6 / §2.18 / §4.16,
// §28.2 / §9.7.

#include "support/aot_support.hpp"
#include "support/sema_support.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff a `where T: …` constraint-not-satisfied diagnostic was reported.
bool hasConstraintError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("does not satisfy the constraint") != std::string::npos)
            return true;
    return false;
}

// A generic static method whose single type parameter T carries `bound`, called
// with `arg` — the method-call constraint-check site.
std::string callSite(std::string_view bound, std::string_view arg,
                     std::string_view argVal) {
    return std::string(
               "module M; "
               "static class F "
               "  public static method Gen<T>(v: T): int where T: ") +
           std::string(bound) +
           " return 0; "
           "  end Gen; "
           "end F; "
           "class C public static method Main(): int "
           "  return F.Gen<" +
           std::string(arg) + ">(" + std::string(argVal) +
           "); "
           "end Main; end C;";
}

}  // namespace

// ── REJECT: wrong generic argument on a synthesized builtin interface ────────
// Fix-critical: accepted before the fix, rejected after.

// §14.2 clause 5 / stdlib §9.1: `int` declares `IComparable<int>`, NOT
// `IComparable<long>` — the wrong generic argument must be rejected.
BEZEL_TEST(primitive_builtin_iface_generic_arg_1867,
           callsite_int_vs_icomparable_long_rejected) {
    auto r = parseSemaAll(callSite("IComparable<long>", "int", "5"));
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

// §14.2 clause 5 / stdlib §2.18: `int` is not an equality *comparer* — no
// primitive conforms to `IEqualityComparer<…>`.
BEZEL_TEST(primitive_builtin_iface_generic_arg_1867,
           callsite_int_vs_iequalitycomparer_rejected) {
    auto r = parseSemaAll(callSite("IEqualityComparer<int>", "int", "5"));
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

// §14.2 clause 5 / stdlib §4.16: `int` is not a span — no primitive conforms to
// `IReadOnlySpan<…>`.
BEZEL_TEST(primitive_builtin_iface_generic_arg_1867,
           callsite_int_vs_ireadonlyspan_rejected) {
    auto r = parseSemaAll(callSite("IReadOnlySpan<int>", "int", "5"));
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

// §14.2 clause 5 + §28.2/§9.7: only classes implement `IDisposable`; a primitive
// never does. (Bound resolves to a declaration-null Named TypeRef.)
BEZEL_TEST(primitive_builtin_iface_generic_arg_1867,
           callsite_int_vs_idisposable_rejected) {
    auto r = parseSemaAll(callSite("IDisposable", "int", "5"));
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

// §14.2 clause 5 / stdlib §9.4: `Boolean` lists `IEquatable<bool>` only — `bool`
// does not conform to `IComparable<bool>`.
BEZEL_TEST(primitive_builtin_iface_generic_arg_1867,
           callsite_bool_vs_icomparable_rejected) {
    auto r = parseSemaAll(callSite("IComparable<bool>", "bool", "true"));
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

// §14.2 clause 5: the same wrong-generic-argument rejection at the generic-TYPE
// instantiation site (`new Box<int>()` where `Box<T> where T: IComparable<long>`).
BEZEL_TEST(primitive_builtin_iface_generic_arg_1867,
           typeinst_int_vs_icomparable_long_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class Box<T> where T: IComparable<long> public _v: T?; end Box; "
        "class C public static method Main(): int "
        "  var b := new Box<int>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

// §14.2 clause 5 + §28.2/§9.7: the same `IDisposable` rejection at the type-
// instantiation site.
BEZEL_TEST(primitive_builtin_iface_generic_arg_1867,
           typeinst_int_vs_idisposable_rejected) {
    auto r = parseSemaAll(
        "module M; "
        "class Box<T> where T: IDisposable public _v: T?; end Box; "
        "class C public static method Main(): int "
        "  var b := new Box<int>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

// §14.2 clause 5: the wrong-generic-argument program no longer compiles end-to-
// end through the REAL AOT driver (fork-isolated compile + link + native exec).
BEZEL_TEST(primitive_builtin_iface_generic_arg_1867,
           callsite_int_vs_icomparable_long_rejected_by_aot) {
    auto r = runAot(callSite("IComparable<long>", "int", "5"));
    BEZEL_EXPECT_TRUE(!r.ran && r.diagnostics.hasErrors());
}

// ── ACCEPT: legitimate conformances must be UNAFFECTED (no false positives) ──

// §14.2 worked example `where T: IComparable<int>` / stdlib §9.1: `int` conforms
// with the correct generic argument. Must stay accepted.
BEZEL_TEST(primitive_builtin_iface_generic_arg_1867,
           callsite_int_vs_icomparable_int_accepted) {
    auto r = parseSemaAll(callSite("IComparable<int>", "int", "5"));
    BEZEL_EXPECT_TRUE(!hasConstraintError(r));
}

// stdlib §9.1: `int` conforms to `IEquatable<int>`. Must stay accepted.
BEZEL_TEST(primitive_builtin_iface_generic_arg_1867,
           callsite_int_vs_iequatable_int_accepted) {
    auto r = parseSemaAll(callSite("IEquatable<int>", "int", "5"));
    BEZEL_EXPECT_TRUE(!hasConstraintError(r));
}

// §14.2 clause 5: the self-referential bound `where T: IComparable<T>` (the
// `Math.Min`/`Sort` pattern, stdlib §2.7 / §4) with T=int — the bound's generic
// argument is the type parameter itself, which the Defer arm must accept.
BEZEL_TEST(primitive_builtin_iface_generic_arg_1867,
           callsite_int_vs_icomparable_self_accepted) {
    auto r = parseSemaAll(callSite("IComparable<T>", "int", "5"));
    BEZEL_EXPECT_TRUE(!hasConstraintError(r));
}

// stdlib §9.4: `bool` conforms to `IEquatable<bool>` (its one declared instance
// interface). Must stay accepted — the `bool` reject is IComparable-only.
BEZEL_TEST(primitive_builtin_iface_generic_arg_1867,
           callsite_bool_vs_iequatable_accepted) {
    auto r = parseSemaAll(callSite("IEquatable<bool>", "bool", "true"));
    BEZEL_EXPECT_TRUE(!hasConstraintError(r));
}

// stdlib §9.6: `string` conforms to `IComparable<string>`. Must stay accepted.
BEZEL_TEST(primitive_builtin_iface_generic_arg_1867,
           callsite_string_vs_icomparable_accepted) {
    auto r = parseSemaAll(callSite("IComparable<string>", "string", "\"x\""));
    BEZEL_EXPECT_TRUE(!hasConstraintError(r));
}

// §3.4 / stdlib §9.1: the PascalCase record form `Int32` names the same type as
// the keyword `int`. `IComparable<Int32>` resolves its argument to a Named (not
// keyword-primitive) TypeRef, so the Defer arm keeps `int` accepted — no over-
// rejection of a spelling that is in fact the correct conformance.
BEZEL_TEST(primitive_builtin_iface_generic_arg_1867,
           callsite_int_vs_icomparable_int32_accepted) {
    auto r = parseSemaAll(callSite("IComparable<Int32>", "int", "5"));
    BEZEL_EXPECT_TRUE(!hasConstraintError(r));
}

// §14.2 clause 5: the correct-conformance control at the type-instantiation site
// (`new Box<int>()` where `Box<T> where T: IComparable<int>`). Must stay accepted.
BEZEL_TEST(primitive_builtin_iface_generic_arg_1867,
           typeinst_int_vs_icomparable_int_accepted) {
    auto r = parseSemaAll(
        "module M; "
        "class Box<T> where T: IComparable<int> public _v: T?; end Box; "
        "class C public static method Main(): int "
        "  var b := new Box<int>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(!hasConstraintError(r));
}

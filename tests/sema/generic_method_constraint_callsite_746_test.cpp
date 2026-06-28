// Spec-derived sema tests for fix #746 (R41) — a generic METHOD's
// `class` / `value` / `notnull` kind constraints are enforced at the call site
// (explicit and inferred type arguments).
//
// Authority: §14.2 — constraints are evaluated per-instantiation, including
// method instantiations. #729 enforced only the `type` (interface/class bound)
// kind; the `class`/`value`/`notnull` kinds were skipped, and a primitive type
// argument (no declaration) was dropped — so `Clone<int>()` with `where T: class`
// was silently accepted, a soundness hole that SIGSEGV'd via a bad dispatch.

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

bool hasConstraintError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("does not satisfy the constraint") !=
                std::string::npos &&
            d.message.find("14.2") != std::string::npos)
            return true;
    return false;
}

const char* kIdClass =
    "static class F "
    "  public static method Id<T>(v: T): T where T: class return v; end Id; "
    "end F; ";

}  // namespace

// §14.2: an EXPLICIT `int` type arg violates `where T: class`.
BEZEL_TEST(method_constraint_callsite_746, explicit_int_violates_class) {
    auto r = parseSemaAll(
        std::string("module M; ") + kIdClass +
        "class C public static method Main(): int "
        "  var r := F.Id<int>(42); return r; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

// §14.2: an INFERRED record (value type) violates `where T: class`.
BEZEL_TEST(method_constraint_callsite_746, inferred_record_violates_class) {
    auto r = parseSemaAll(
        std::string("module M; record W(x: int); ") + kIdClass +
        "class C public static method Main(): int "
        "  var w := new W(7); var r := F.Id(w); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

// §14.2: a class type argument SATISFIES `where T: class` — no error.
BEZEL_TEST(method_constraint_callsite_746, class_satisfies_class) {
    auto r = parseSemaAll(
        std::string("module M; class Widget public n: int := 0; end Widget; ") +
        kIdClass +
        "class C public static method Main(): int "
        "  using var w := new Widget(); var r := F.Id(w); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
}

// §14.2: a class type argument violates `where T: value`.
BEZEL_TEST(method_constraint_callsite_746, class_violates_value) {
    auto r = parseSemaAll(
        "module M; class Widget public n: int := 0; end Widget; "
        "static class F "
        "  public static method Id<T>(v: T): T where T: value return v; end Id; "
        "end F; "
        "class C public static method Main(): int "
        "  using var w := new Widget(); var r := F.Id(w); return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
}

// §14.2: an `int` (a value type) SATISFIES `where T: value` — no error.
BEZEL_TEST(method_constraint_callsite_746, int_satisfies_value) {
    auto r = parseSemaAll(
        "module M; "
        "static class F "
        "  public static method Id<T>(v: T): T where T: value return v; end Id; "
        "end F; "
        "class C public static method Main(): int "
        "  var r := F.Id<int>(42); return r; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(hasConstraintError(r));
}

// ---------------------------------------------------------------------------
// §14.2 (#746): contradictory constraint combinations (`class` + `value`) are
// compile errors — checked at the DECLARATION, independent of instantiation.
// ---------------------------------------------------------------------------

namespace {
bool hasContradictionError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("contradictory constraints") != std::string::npos)
            return true;
    return false;
}
}  // namespace

// §14.2: a generic TYPE with `where T: class, value` is rejected (even though
// it is never instantiated).
BEZEL_TEST(method_constraint_callsite_746, contradictory_type_constraint) {
    auto r = parseSemaAll(
        "module M; class Foo<T> where T: class, value end Foo; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(hasContradictionError(r));
}

// §14.2: a generic METHOD with `where T: class, value` is rejected.
BEZEL_TEST(method_constraint_callsite_746, contradictory_method_constraint) {
    auto r = parseSemaAll(
        "module M; static class F "
        "  public static method Do<T>(v: T): int where T: class, value "
        "    return 0; end Do; "
        "end F; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(hasContradictionError(r));
}

// §14.2: a single (non-contradictory) constraint is accepted.
BEZEL_TEST(method_constraint_callsite_746, single_constraint_ok) {
    auto r = parseSemaAll(
        "module M; class Foo<T> where T: class end Foo; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(hasContradictionError(r));
}

// ---------------------------------------------------------------------------
// §14.2 / fix #1078: method call on an UNCONSTRAINED type parameter is rejected.
// An unconstrained T has only Object members (ToString/Equals/GetHashCode/
// GetType); any call to a non-Object member on a bare T must be a sema error
// ("no member `M` on unconstrained type parameter `T`"). The constrained case
// (where T: ISomething) still resolves through the constraint and must pass.
// ---------------------------------------------------------------------------

namespace {
bool hasUnconstrainedParamError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("unconstrained type parameter") != std::string::npos)
            return true;
    return false;
}
}  // namespace

// §14.2 (#1078): a method call on an unconstrained class-level T field is
// rejected — T provides no member `Value`, so this is ill-formed.
BEZEL_TEST(method_constraint_callsite_746,
           unconstrained_class_field_t_method_rejected_1078) {
    // Holder<T> (no constraint); _item.Value() on unconstrained T must error.
    auto r = parseSemaAll(
        "module M; "
        "class Holder<T> "
        "  private _item: T?; "
        "  public method GetValue(): int _item!.Value(); return 0; end GetValue; "
        "end Holder; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(hasUnconstrainedParamError(r));
}

// §14.2 (#1078): a method call on an unconstrained generic METHOD parameter T
// is rejected — the method's `x: T` (no where-clause) provides no `GetN()`.
BEZEL_TEST(method_constraint_callsite_746,
           unconstrained_method_param_t_method_rejected_1078) {
    auto r = parseSemaAll(
        "module M; "
        "static class U "
        "  public static method Wrap<T>(x: T): int x.GetN(); return 0; end Wrap; "
        "end U; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(hasUnconstrainedParamError(r));
}

// §14.2 (#1078): ANTI-OVER-REJECT — a CONSTRAINED T (`where T: IFoo`) method
// call on member F() provided by IFoo must still be ACCEPTED (no error).
BEZEL_TEST(method_constraint_callsite_746,
           constrained_t_iface_member_accepted_1078) {
    auto r = parseSemaAll(
        "module M; "
        "interface IFoo method F(): int; end IFoo; "
        "class Holder<T> where T: IFoo "
        "  private _item: T?; "
        "  public method GetF(): int return _item!.F(); end GetF; "
        "end Holder; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(hasUnconstrainedParamError(r));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §14.2 (#1078): ANTI-OVER-REJECT — Object members (ToString, GetHashCode)
// on an unconstrained T are valid and must NOT be rejected.  Tests that the
// new `else` branch only fires for non-Object members.
BEZEL_TEST(method_constraint_callsite_746,
           object_members_on_unconstrained_t_accepted_1078) {
    // ToString() and GetHashCode() are the clearest Object-provided members
    // (§14.1); calling them on unconstrained T must not trigger §14.2 reject.
    auto r = parseSemaAll(
        "module M; "
        "static class U "
        "  public static method CheckObj<T>(x: T): int "
        "    var s := x.ToString(); "
        "    var h := x.GetHashCode(); "
        "    return h; "
        "  end CheckObj; "
        "end U; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(hasUnconstrainedParamError(r));
}

// §7.1 / §14.2 (#741 f13): a nullable type argument that violates a `value`
// constraint must be NAMED with its `?` in the diagnostic. The constraint was
// always correctly enforced; only the displayed name was wrong (`int` for the
// `int?` argument), because argNameOf dropped the nullable flag.
BEZEL_TEST(method_constraint_callsite_746, nullable_arg_name_shows_question_mark) {
    auto r = parseSemaAll(
        "module M; class F "
        "  public static method ValueOnly<T>(x: T): T where T: value "
        "    return x; end ValueOnly; "
        "end F; "
        "class C public static method Main(): int "
        "  var v: int? := 5; var r := F.ValueOnly<int?>(v!); return r!; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasConstraintError(r));
    bool named = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("`int?`") != std::string::npos) named = true;
    BEZEL_EXPECT_TRUE(named);
}

// Spec-derived tests for issue #414 — diagnose an unknown member access on a
// fully-resolved USER type (a `class`/`record`/`enum` whose member set sema
// knows completely).
//
// Spec basis. A member access `obj.M` (and a method call `obj.M(...)`) must
// resolve to a member that the receiver's type actually has (§11.3 lists the
// members a class body declares; §12.1/§12.3 govern how a member name resolves
// against a type and its bases). Every type additionally has the universal
// `Object` members `GetType`/`Equals`/`GetHashCode`/`ToString`/`ReferenceEquals`
// (§8.1) and — for non-static classes — `Dispose` (§8.7). A name that matches
// none of these on a known-complete user type resolves to nothing; before this
// fix sema left such an access unresolved and un-diagnosed, and codegen then
// lowered a null callee to a garbage value (silent miscompile). This pass turns
// the genuinely-unknown case into a compile error.
//
// SCOPE (deliberately narrow to avoid over-rejection — see #414). The
// diagnostic fires ONLY when the receiver's static type is a fully-resolved
// user `class`/`record`/`enum` (a Named TypeRef with a non-null `declaration`
// that is a user TypeDecl). It STAYS PERMISSIVE for: builtin/intrinsic types
// (Array/Dictionary/string/Console/Math/Task/…, whose TypeRef carries a null
// declaration), `Object` (a primitive lookup key, not a Named user type),
// interface-typed receivers, generic type parameters (Named with null
// declaration), and any receiver whose type did not resolve. The universal
// Object/IDisposable members are whitelisted so they are never falsely rejected
// on a user type.

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

// True iff some emitted diagnostic is the #414 unknown-member message AND it
// names both the offending member and the type. The message format (see
// ExprTypecheck_Calls.cpp) is "no member `<m>` on type `<T>` …".
bool hasUnknownMemberError(const bezel::test::SemaResult& r,
                           std::string_view member, std::string_view type) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("no member") == std::string::npos) continue;
        if (d.message.find(std::string("`") + std::string(member) + "`") ==
            std::string::npos)
            continue;
        if (d.message.find(std::string("`") + std::string(type) + "`") ==
            std::string::npos)
            continue;
        return true;
    }
    return false;
}

} // namespace

// ===========================================================================
// POSITIVE — the diagnostic must fire (load-bearing: these FAIL to diagnose
// before the fix). Each cites the member and the user type.
// ===========================================================================

// §11.3 / §12.1: a field name that no member of the class has.
BEZEL_TEST(sema_unknown_member, unknown_field_on_class_diagnosed) {
    auto r = parseSemaAll(
        "module M; "
        "class Animal public _legs: int; end Animal; "
        "class C "
        "  public method M(a: Animal): int => a.NoSuchField; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasUnknownMemberError(r, "NoSuchField", "Animal"));
}

// §11.3 / §12.1: a method name that no member of the class has.
BEZEL_TEST(sema_unknown_member, unknown_method_on_class_diagnosed) {
    auto r = parseSemaAll(
        "module M; "
        "class Animal public method Speak(): int => 1; end Animal; "
        "class C "
        "  public method M(a: Animal): int => a.NoSuchMethod(); "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasUnknownMemberError(r, "NoSuchMethod", "Animal"));
}

// §12.1: a typo of a real member (`Spea` for `Speak`) is unresolved → error.
BEZEL_TEST(sema_unknown_member, typo_of_real_method_diagnosed) {
    auto r = parseSemaAll(
        "module M; "
        "class Animal public method Speak(): int => 1; end Animal; "
        "class C "
        "  public method M(a: Animal): int => a.Spea(); "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasUnknownMemberError(r, "Spea", "Animal"));
}

// §9 record: an unknown field on a positional record.
BEZEL_TEST(sema_unknown_member, unknown_field_on_record_diagnosed) {
    auto r = parseSemaAll(
        "module M; "
        "record Point(x: int, y: int); "
        "class C "
        "  public method M(p: Point): int => p.z; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasUnknownMemberError(r, "z", "Point"));
}

// §10 enum: an unknown member name accessed statically on the enum type.
BEZEL_TEST(sema_unknown_member, unknown_constant_on_enum_diagnosed) {
    auto r = parseSemaAll(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "class C "
        "  public method M(): int "
        "    var c := Color.Purple; "
        "    return 0; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasUnknownMemberError(r, "Purple", "Color"));
}

// §11.3: `this.<unknown>` — the receiver is the current user class.
BEZEL_TEST(sema_unknown_member, unknown_member_on_this_diagnosed) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private _x: int; "
        "  public method M(): int => this.bogus; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasUnknownMemberError(r, "bogus", "C"));
}

// §11.3: an unknown STATIC member accessed via the type name.
BEZEL_TEST(sema_unknown_member, unknown_static_member_on_type_diagnosed) {
    auto r = parseSemaAll(
        "module M; "
        "class Other public static _total: int := 5; end Other; "
        "class C "
        "  public method M(): int => Other.NoSuchStatic; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasUnknownMemberError(r, "NoSuchStatic", "Other"));
}

// ===========================================================================
// NEGATIVE — must STILL compile (zero over-rejection). These pass both before
// and after the fix.
// ===========================================================================

// Real field / method / property on a user class resolve cleanly.
BEZEL_TEST(sema_unknown_member, real_members_on_class_clean) {
    auto r = parseSemaAll(
        "module M; "
        "class Animal "
        "  public _legs: int; "
        "  public property Name: int; "
        "  public method Speak(): int => 1; "
        "end Animal; "
        "class C "
        "  public method M(a: Animal): int => a._legs + a.Name + a.Speak(); "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §12: an inherited member (declared on a base class) resolves cleanly.
BEZEL_TEST(sema_unknown_member, inherited_member_on_class_clean) {
    auto r = parseSemaAll(
        "module M; "
        "class Base public method Tick(): int => 1; end Base; "
        "class Derived : Base end Derived; "
        "class C "
        "  public method M(d: Derived): int => d.Tick(); "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §8.1 / §8.5: the universal Object members are valid on a user CLASS even
// without a user-declared override (inherited from Object) — must not reject.
BEZEL_TEST(sema_unknown_member, object_members_on_class_clean) {
    auto r = parseSemaAll(
        "module M; "
        "class Widget public _id: int; end Widget; "
        "class C "
        "  public method M(w: Widget): int "
        "    var s := w.ToString(); "
        "    var h := w.GetHashCode(); "
        "    var e := w.Equals(w); "
        "    return h; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §8.1 / §8.5: the universal Object members are valid on a positional record
// (synthesized value-semantics members) — must not reject.
BEZEL_TEST(sema_unknown_member, object_members_on_record_clean) {
    auto r = parseSemaAll(
        "module M; "
        "record Point(x: int, y: int); "
        "class C "
        "  public method M(p: Point): int "
        "    var s := p.ToString(); "
        "    var h := p.GetHashCode(); "
        "    return h; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §8.7: `Dispose()` is available on every non-static class (IDisposable),
// even with no user-declared Dispose — must not reject.
BEZEL_TEST(sema_unknown_member, dispose_on_class_clean) {
    auto r = parseSemaAll(
        "module M; "
        "class Handle public _fd: int; end Handle; "
        "class C "
        "  public method M(h: Handle) h.Dispose(); end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Builtin collection methods (no sema MemberSymbol) stay permissive (#420/#421).
BEZEL_TEST(sema_unknown_member, builtin_collection_methods_clean) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method M(a: Array<int>): int "
        "    a.Add(1); "
        "    var n := a.Count; "
        "    var f := a.First(); "
        "    return n + f; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Builtin string methods (no sema MemberSymbol) stay permissive (§9.6).
BEZEL_TEST(sema_unknown_member, builtin_string_methods_clean) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method M(s: string): int "
        "    var t := s.Trim(); "
        "    var u := s.ToUpper(); "
        "    return s.Length; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Builtin static classes Console / Math stay permissive (§2.3 / stdlib §9).
BEZEL_TEST(sema_unknown_member, builtin_console_math_clean) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method M(): int "
        "    Console.WriteLine(\"hi\"); "
        "    var x := Math.Sqrt(4.0); "
        "    return 0; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §3: a member access on an `Object`-typed receiver (a primitive lookup key,
// not a Named user type) stays permissive — the static type carries no
// known-complete member set.
BEZEL_TEST(sema_unknown_member, object_typed_receiver_permissive) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method M(o: Object): int "
        "    var s := o.Anything(); "
        "    return 0; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// An interface-typed receiver stays permissive — interfaces are not treated as
// known-complete here (default/inherited member resolution is incomplete).
BEZEL_TEST(sema_unknown_member, interface_typed_receiver_permissive) {
    auto r = parseSemaAll(
        "module M; "
        "interface IShape method Area(): int; end IShape; "
        "class C "
        "  public method M(s: IShape): int => s.SomethingNotDeclared(); "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A real interface member resolves cleanly on an interface-typed receiver.
BEZEL_TEST(sema_unknown_member, interface_real_member_clean) {
    auto r = parseSemaAll(
        "module M; "
        "interface IShape method Area(): int; end IShape; "
        "class C "
        "  public method M(s: IShape): int => s.Area(); "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §14.1: a generic type parameter `T` (Named with null declaration) stays
// permissive — its member set is not statically known.
BEZEL_TEST(sema_unknown_member, generic_param_receiver_permissive) {
    auto r = parseSemaAll(
        "module M; "
        "class Box<T> "
        "  public method M(t: T): int "
        "    var s := t.Whatever(); "
        "    return 0; "
        "  end M; "
        "end Box;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §12.7: a member declared only on the narrowed (derived) type resolves after
// an `is` narrowing — and must not be falsely rejected against the static type.
// (Guards the #413 interaction: the diagnostic must run against the narrowed
// type, not the declared base.)
BEZEL_TEST(sema_unknown_member, narrowed_type_member_clean) {
    auto r = parseSemaAll(
        "module M; "
        "class Animal public method Speak(): int => 0; end Animal; "
        "class Dog : Animal public method Bark(): int => 1; end Dog; "
        "class C "
        "  public method M(a: Animal): int "
        "    if a is Dog then "
        "      return a.Bark(); "
        "    end; "
        "    return 0; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

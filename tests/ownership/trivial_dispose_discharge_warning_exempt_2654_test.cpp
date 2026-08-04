// Spec-derived tests for issue #2654 — §28.9's trivial-dispose exemption from
// the §28.3 ownership-discharge (auto-drop must-use) warning. Spec v0.237.
//
// §28.9 "Trivial-dispose classes", first bullet, VERBATIM:
//
//     "A `new` expression of a trivial-dispose class type whose result is not
//      stored in an `owned` location or `using var` does *not* produce the
//      standard ownership-discharge warning. (There is nothing to discharge.)"
//
// (Restated at Appendix C.3.) §28.9's "Trivial-dispose classification is part
// of the API surface" states the same fact from the other side: promoting a
// class OUT of trivial-dispose means "an unused instance that dropped silently
// now draws the must-use auto-drop warning (§28.3)" — i.e. while the class IS
// trivial-dispose, the unused instance drops SILENTLY.
//
// Before the fix, the auto-drop gate in the ownership pass never consulted
// trivial-dispose, so `var b := new Box();` (Box trivial-dispose) drew the
// §28.3 warning anyway.
//
// ⚠ SCOPE OF THE FIX — the invariant this file exists to protect:
// the fix suppresses the WARNING ONLY. `LocalDecl::autoDropAtScopeEnd` is
// still set, so codegen still emits exactly the same scope-end free. That
// separation is load-bearing: trivial-dispose only makes `Dispose()` provably
// a no-op, while §28.9 still requires the instance's MEMORY be reclaimed
// ("Memory for trivial-dispose instances is reclaimed by an implementation-
// defined mechanism"). Dropping the free instead of the warning would be a
// LEAK, and dropping it on a shape that keeps a second owner would be a
// DOUBLE-FREE. `trivial_dispose_new_local_still_auto_drops` below pins the
// flag directly for exactly that reason — do not delete it when touching the
// diagnostic.
//
// The negative cases are the false-negative controls: a suppression bug that
// swallowed the warning for genuinely non-trivial classes would hide a real
// §28.3 obligation, which is the more dangerous direction.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasAutoDropWarning;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// A destructor-bearing resource — the canonical NON-trivial-dispose class
// (§28.9: a declared destructor excludes the class from trivial-dispose).
constexpr const char* kRes =
    "class Res public constructor() end constructor; "
    "  destructor end destructor; end Res; ";

std::string wrap(const std::string& decls, const std::string& mainBody) {
    return std::string("module M; ") + decls +
           "class C public static method Main(): int " + mainBody +
           " end Main; end C;";
}

// Walks the parsed AST for the top-level `LocalDecl` named `name` in `Main`
// and reports its `autoDropAtScopeEnd` flag — the flag codegen reads to emit
// the scope-end free. `found` distinguishes "flag is false" from "no such
// local", so a renamed fixture cannot make this assertion vacuous.
bool autoDropFlagOf(const OwnershipResult& r, std::string_view name,
                    bool* found) {
    *found = false;
    if (!r.sourceFile) return false;
    for (const auto& td : r.sourceFile->topDecls) {
        auto* type = dynamic_cast<const bezel::ast::TypeDecl*>(td.get());
        if (!type) continue;
        for (const auto& m : type->members) {
            auto* md = dynamic_cast<const bezel::ast::MethodDecl*>(m.get());
            if (!md) continue;
            for (const auto& s : md->stmts) {
                auto* ld = dynamic_cast<const bezel::ast::LocalDecl*>(s.get());
                if (!ld || ld->name != name) continue;
                *found = true;
                return ld->autoDropAtScopeEnd;
            }
        }
    }
    return false;
}

}  // namespace

// =============================================================================
// POSITIVE — the exemption applies (the bug in #2654).
// =============================================================================

// §28.9 (v0.237) first bullet, the issue's own repro: `Box` has no destructor,
// no `owned` field and no base, so it is trivial-dispose; `var b` is a plain
// local, neither an `owned` location nor a `using var`. No warning is due.
BEZEL_TEST(own_trivial_dispose_2654, new_local_of_trivial_class_is_exempt) {
    auto r = parseAndCheckOwnership(wrap(
        "class Box public constructor() end constructor; "
        "  public Value: int := 7; end Box; ",
        "var b := new Box(); return b.Value;"));
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "b"));
}

// ⚠ The scope guard. §28.9 exempts the WARNING; it does not relax the
// requirement that the instance's memory be reclaimed. `autoDropAtScopeEnd`
// must therefore still be set on the very local whose warning we suppressed —
// codegen keys the scope-end free on this flag. Suppressing the flag instead
// of the diagnostic would leak every trivial-dispose local. (Verified at the
// object-code level when the fix landed: `bezelc -c` produced a byte-identical
// object file before and after.)
BEZEL_TEST(own_trivial_dispose_2654, trivial_dispose_new_local_still_auto_drops) {
    auto r = parseAndCheckOwnership(wrap(
        "class Box public constructor() end constructor; "
        "  public Value: int := 7; end Box; ",
        "var b := new Box(); return b.Value;"));
    bool found = false;
    bool flag = autoDropFlagOf(r, "b", &found);
    BEZEL_EXPECT_TRUE(found);
    BEZEL_EXPECT_TRUE(flag);
    // Both halves at once: the free is scheduled AND the diagnostic is gone.
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "b"));
}

// §28.9 "Trivial-dispose classes": "…and its base class (if any, other than
// `Object`) is itself trivial-dispose". A class derived from a trivial base,
// adding nothing, is trivial-dispose too.
BEZEL_TEST(own_trivial_dispose_2654, derived_from_trivial_base_is_exempt) {
    auto r = parseAndCheckOwnership(wrap(
        "class Box public constructor() end constructor; "
        "  public Value: int := 7; end Box; "
        "class SubBox : Box public constructor() end constructor; end SubBox; ",
        "var b := new SubBox(); return b.Value;"));
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "b"));
}

// §28.9 "Trivial-dispose for generic class instantiations" — the spec's own
// `Box<T>` example: "public class Box<T>(value: T) … No destructor, no owned
// fields. Box<T> is trivial-dispose for any T."
BEZEL_TEST(own_trivial_dispose_2654, generic_trivial_instantiation_is_exempt) {
    auto r = parseAndCheckOwnership(wrap(
        "class GBox<T>(value: T) public Value: T := value; end GBox; ",
        "var b := new GBox<int>(3); return b.Value;"));
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "b"));
}

// §22.12 rule 4 / §22.13: a `static owned` field is disposed at process-exit
// order, not by an instance's `Dispose()`, so it does not make the class
// non-trivial-dispose. (Pins the shared predicate's existing static carve-out
// through the new gate.)
BEZEL_TEST(own_trivial_dispose_2654, static_owned_field_only_is_exempt) {
    auto r = parseAndCheckOwnership(wrap(
        std::string(kRes) +
        "class S public constructor() end constructor; "
        "  public static owned R: Res? := null; "
        "  public Value: int := 4; end S; ",
        "var b := new S(); return b.Value;"));
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "b"));
}

// =============================================================================
// NEGATIVE — the false-negative controls. Each class is NON-trivial-dispose,
// so §28.9's exemption does not reach it and the §28.3 warning must still fire.
// =============================================================================

// §28.9: a declared destructor excludes the class from trivial-dispose.
BEZEL_TEST(own_trivial_dispose_2654, destructor_class_still_warns) {
    auto r = parseAndCheckOwnership(
        wrap(kRes, "var b := new Res(); return 0;"));
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "b"));
}

// §28.9: an `owned` field excludes the class from trivial-dispose.
BEZEL_TEST(own_trivial_dispose_2654, owned_field_class_still_warns) {
    auto r = parseAndCheckOwnership(wrap(
        std::string(kRes) +
        "class H public constructor() end constructor; "
        "  public owned R: Res? := null; end H; ",
        "var b := new H(); return 0;"));
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "b"));
}

// §22.10 (v0.180): an `owned` primary-constructor parameter referenced in a
// BODY position is captured into an `owned` private field disposed by the
// generated `Dispose()` — so the class is not trivial-dispose.
BEZEL_TEST(own_trivial_dispose_2654, owned_primary_ctor_param_still_warns) {
    auto r = parseAndCheckOwnership(wrap(
        std::string(kRes) +
        "class Holder(owned r: Res) "
        "  public method Touch(): int var x := r; return 0; end Touch; "
        "end Holder; ",
        "var b := new Holder(new Res()); return 0;"));
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "b"));
}

// §28.9: "…and its base class (if any, other than `Object`) is itself
// trivial-dispose" — a destructor-bearing BASE disqualifies the derived class,
// so the base-chain walk must reach it.
BEZEL_TEST(own_trivial_dispose_2654, derived_from_destructor_base_still_warns) {
    auto r = parseAndCheckOwnership(wrap(
        std::string(kRes) +
        "class D : Res public constructor() end constructor; end D; ",
        "var b := new D(); return 0;"));
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "b"));
}

// §28.9's trivial-dispose definition also excludes a class with "no fields of
// owning-inline-array type (`[N owned]T?` or `[N owned]T`, §15.1.1)". Such a
// field carries a real per-slot disposal obligation. The shared
// `common::isNonTrivialDisposeClass` walk does NOT test this arm (pre-existing
// gap, #2666); the §28.3 gate guards it separately so the SUPPRESSION stays
// sound. This test is what keeps that guard honest — it fails if the guard is
// removed before #2666 lands.
BEZEL_TEST(own_trivial_dispose_2654, owning_inline_array_field_still_warns) {
    auto r = parseAndCheckOwnership(wrap(
        std::string(kRes) +
        "class SlotHolder public constructor() end constructor; "
        "  public slots: [2 owned]Res? := [new Res(), new Res()]; "
        "end SlotHolder; ",
        "var b := new SlotHolder(); return 0;"));
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "b"));
}

// ⚠ The subtlest control. §28.9's sentence classifies "a `new` expression OF A
// TRIVIAL-DISPOSE CLASS TYPE" — the type of the EXPRESSION, not the declared
// type of the local it initializes. `var b: Base := new Derived();` with a
// trivial `Base` and a destructor-bearing `Derived` allocates a NON-trivial
// instance that carries a real §28.3 obligation, so the warning is due even
// though the declared type is trivial-dispose. An implementation that keyed
// the exemption on `LocalDecl::type` would silently swallow it.
BEZEL_TEST(own_trivial_dispose_2654, declared_trivial_base_with_nontrivial_new_warns) {
    auto r = parseAndCheckOwnership(wrap(
        "class Base public constructor() end constructor; end Base; "
        "class Derived : Base public constructor() end constructor; "
        "  destructor end destructor; end Derived; ",
        "var b: Base := new Derived(); return 0;"));
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "b"));
}

// The companion: when BOTH the declared type and the allocated type are
// trivial-dispose, the exemption applies through the widening binding. (This
// is the shape several ported lit fixtures use, e.g.
// tests/lit/sema/sema_named__derived_to_base_ok.bzl.)
BEZEL_TEST(own_trivial_dispose_2654, declared_trivial_base_with_trivial_new_is_exempt) {
    auto r = parseAndCheckOwnership(wrap(
        "class Base public constructor() end constructor; end Base; "
        "class Derived : Base public constructor() end constructor; end Derived; ",
        "var b: Base := new Derived(); return 0;"));
    BEZEL_EXPECT_FALSE(hasAutoDropWarning(r, "b"));
}

// §28.9's exemption names the `new` EXPRESSION form specifically. A call to a
// factory returning `owned T` is not that shape, so it keeps today's warning
// even when T is trivial-dispose. Recorded deliberately (CLAUDE.md rule 4):
// the narrow reading is the spec-verbatim one, and a retained warning is noise
// where a wrongly-suppressed one would hide an obligation. Widening this is a
// spec question, not an implementation choice.
BEZEL_TEST(own_trivial_dispose_2654, owned_returning_factory_call_still_warns) {
    auto r = parseAndCheckOwnership(wrap(
        "class Box public constructor() end constructor; "
        "  public Value: int := 7; end Box; "
        "class F public static method MakeBox(): owned Box "
        "  return new Box(); end MakeBox; end F; ",
        "var b := F.MakeBox(); return b.Value;"));
    BEZEL_EXPECT_TRUE(hasAutoDropWarning(r, "b"));
}

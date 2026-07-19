// Spec-derived tests for #2048 — the CALLEE (declaration-site) half of §28.3
// "Factory methods and sourceless returns" (v0.201, proposal 0048).
//
// §28.3 "Factory methods and sourceless returns" (docs/Bezel_Language_Spec.md,
// v0.201):
//
//   "A `static` method or `operator` declaration — wherever it occurs: a
//   class, a `static` class (§11.7), a record, an enum, or an interface's
//   static member set (§11.8) — whose declared return type, nullability
//   stripped, is a class or interface type is a compile error unless one of
//   the following holds:
//     - the return type carries `owned`;
//     - the declared type is a `Task`-family type;
//     - the declared type is one of the iterable or enumerator interfaces;
//     - the declared type is a bare type parameter with a non-empty
//       potential source set.
//   An empty-set bare-type-parameter return is rejected exactly as a
//   concrete class type is: spell `owned T`."
//
// This retires the v0.60 treated-owned-with-a-warning heuristic (#1600).
// The declaration-site rule is now an ERROR (Severity::Error), not a nudge
// warning — implemented in bezel::ownership::checkStaticMethodReturnDeclaration
// (Ownership.cpp), with the signature-keyed predicate
// `staticReturnTypeRejected`. The rule's body-independence is the v0.201
// soundness core: the caller-side classification is read off the declared
// return type with no heuristic, so the producers-table row above never
// classifies a static call. The v0.60 kernel axiom A1 (the treated-owned
// classification) is tombstoned (rationale §5.7, v0.201).
//
// The diagnostic teaches the fix per proposal 0048 §2.3 — two remedies:
// (1) add `owned` (or `owned T`) to the return type, (2) for a cached
// instance the declaring type keeps alive, re-expose it through a static
// property whose getter returns the `static owned` field (§28.6).
//
// "Iterator methods are outside the static-return error above" (§28.3, same
// section, reframed as membership in the announced set v0.201): the iterable
// interface IS the announcement, so no `owned` marker is required.

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// §28.3 v0.201 (#2048): true when the ownership pass emitted the declaration-
// site static-return ERROR — distinguished from the auto-drop must-use
// WARNING (`hasAutoDropWarning`, tests/support/ownership_support.hpp) and from
// the v0.196 borrow-return error (`hasStaticPropertyRemedyError`,
// sourceless_static_factory_1499_test.cpp, which matches on "static property"
// in an ERROR diagnostic). The fingerprint unique to THIS check is the
// "without `owned`" + "Factory methods and sourceless returns" reference.
bool hasStaticReturnError(const bezel::test::OwnershipResult& r,
                          const std::string& name = "") {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("without `owned`") == std::string::npos)
            continue;
        if (d.message.find("Factory methods and sourceless returns") ==
            std::string::npos)
            continue;
        if (name.empty() ||
            d.message.find("`" + name + "`") != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// --- Positive: the exact §28.3 / #1499 "Make(): Res" shape is now REJECTED
// at the declaration (was a WARNING under the v0.60 heuristic; v0.201 flips
// the severity). The body content is no longer consulted — the signature
// itself fails the announced-set rule. ---

BEZEL_TEST(static_factory_owned_nudge_1600, sourceless_static_factory_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class Res "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Factory "
        "  public static method Make(): Res return new Res(); end Make; "
        "  public static method Main(): int "
        "    using var m := Make(); "
        "    return 0; "
        "  end Main; "
        "end Factory;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasStaticReturnError(r, "Make"));
}

// The declaration-site rule is independent of any caller's discharge shape —
// it fires whether the call is `var m := Make();`, `using var m := Make();`,
// or never called at all. (Was the v0.60 warning's "declaration-level lint"
// property; v0.201 carries it forward as the declaration-level ERROR.)
BEZEL_TEST(static_factory_owned_nudge_1600,
           fires_regardless_of_caller_discharge_style) {
    auto r = parseAndCheckOwnership(
        "module M; class Res "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Factory "
        "  public static method Make(): Res return new Res(); end Make; "
        "  public static method Main(): int "
        "    using var m := Make(); "
        "    return 0; "
        "  end Main; "
        "end Factory;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasStaticReturnError(r, "Make"));
}

// Expression-bodied static factories (`Make(): Res => new Res();`) are
// rejected identically — the predicate is signature-keyed (bodyKind-
// independent).
BEZEL_TEST(static_factory_owned_nudge_1600,
           expression_bodied_sourceless_factory_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; class Res "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Factory "
        "  public static method Make(): Res => new Res(); "
        "  public static method Main(): int "
        "    using var m := Make(); "
        "    return 0; "
        "  end Main; "
        "end Factory;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasStaticReturnError(r, "Make"));
}

// --- The rule is BODY-INDEPENDENT (the v0.201 signature-keyed core): a
// static-storage-sourced BORROW return is ALSO rejected at the declaration,
// not only by the v0.196 body-side `checkStaticBorrowReturn`. The body-side
// error still fires too; this test confirms the declaration-site error is
// present on top. ---

BEZEL_TEST(static_factory_owned_nudge_1600,
           borrow_field_return_also_rejected_at_declaration) {
    auto r = parseAndCheckOwnership(
        "module M; class Res "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Holder "
        "  public static readonly _shared: Res := new Res(); "
        "  public static method Get(): Res => _shared; "
        "  public static method Main(): int "
        "    var r := Get(); "
        "    return 0; "
        "  end Main; "
        "end Holder;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasStaticReturnError(r, "Get"));
}

// --- Negative: already-annotated `owned` return needs no error. ---

BEZEL_TEST(static_factory_owned_nudge_1600, owned_return_no_error) {
    auto r = parseAndCheckOwnership(
        "module M; class Res "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Factory "
        "  public static method Make(): owned Res return new Res(); end Make; "
        "  public static method Main(): int "
        "    using var m := Make(); "
        "    return 0; "
        "  end Main; "
        "end Factory;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasStaticReturnError(r, "Make"));
}

// --- Negative: an INSTANCE method has a structural source (the receiver) —
// §28.3 producers table ("instance method: source is the receiver"). ---

BEZEL_TEST(static_factory_owned_nudge_1600, instance_method_no_error) {
    auto r = parseAndCheckOwnership(
        "module M; class Res "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Holder "
        "  public owned _r: Res; "
        "  public constructor(owned r: Res) _r := r; end constructor; "
        "  destructor end destructor; "
        "  public method Get(): Res => _r; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    var h := new Holder(new Res()); "
        "    var b := h.Get(); "
        "    h.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasStaticReturnError(r, "Get"));
}

// --- Negative: a value-typed return carries no ownership obligation at all —
// the announced-set / class-or-interface guard excludes it. ---

BEZEL_TEST(static_factory_owned_nudge_1600, value_typed_return_no_error) {
    auto r = parseAndCheckOwnership(
        "module M; class Factory "
        "  public static method Make(): int return 5; end Make; "
        "  public static method Main(): int "
        "    var m := Make(); "
        "    return 0; "
        "  end Main; "
        "end Factory;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasStaticReturnError(r, "Make"));
}

// --- Negative (v0.201 type-keyed iterator carve): iterator methods are
// outside the rule because their iterable-interface return type IS the
// announcement — no `owned` marker needed. ---

BEZEL_TEST(static_factory_owned_nudge_1600, iterator_method_no_error) {
    auto r = parseAndCheckOwnership(
        "module M; class Box "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class Gen "
        "  public static method Make(): IOwningEnumerable<Box> "
        "    yield return new Box(); "
        "  end Make; "
        "  public static method Main(): int "
        "    for var b in Make() do "
        "      b.Dispose(); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Gen;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasStaticReturnError(r, "Make"));
}

// --- Negative (v0.201 announced-set carve): a `Task`-family return type is
// itself the ownership-transferring announcement (§28.3 *Task-family calls
// produce owned frames*, v0.190) — no `owned` marker required. ---

BEZEL_TEST(static_factory_owned_nudge_1600, task_family_return_no_error) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Make(): Task<int> "
        "    return Task.FromResult(0); "
        "  end Make; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasStaticReturnError(r, "Make"));
}

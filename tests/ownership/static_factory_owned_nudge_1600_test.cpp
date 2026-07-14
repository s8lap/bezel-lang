// Spec-derived tests for #1600 — the CALLEE (declaration-site) half of §28.3
// "Factory methods and sourceless returns" that #1499 left unimplemented.
//
// §28.3 "Factory methods and sourceless returns" (docs/Bezel_Language_Spec.md):
// "When the compiler cannot identify a source for a non-`owned` class-typed
// return from a static method, it treats the return as owned and emits a
// warning recommending that the callee add `owned` to its return type."
//
// This is a SECOND, distinct diagnostic from the caller-side §28.3 auto-drop
// must-use warning that #1499 already produces (once a call is classified
// owned, an undischarged local at the call site gets its own warning). This
// one fires ONCE PER METHOD DECLARATION — a lint on the API's shape,
// independent of whether/how many times the factory is called — implemented
// in bezel::ownership::checkStaticFactoryNudgeWarning (Ownership.cpp),
// reusing common::staticFactoryReturnsFreshOwned (the same predicate #1499's
// caller-side classification consults) so the nudge fires in exactly the
// cases where that caller-side rule applies.
//
// "Iterator methods are exempt from this warning" (§28.3, same section,
// §29.3 cross-reference): "adding `owned` to every iterator return type
// would be redundant noise."

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// §28.3 (fix #1600): true when the ownership pass emitted the declaration-
// site "add `owned`" nudge WARNING — distinct from the auto-drop must-use
// warning (`hasAutoDropWarning`, tests/support/ownership_support.hpp), which
// matches on "is implicitly disposed at the end of" instead.
bool hasFactoryNudgeWarning(const bezel::test::OwnershipResult& r,
                            const std::string& methodName = "") {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("sourceless-return rule") == std::string::npos)
            continue;
        if (methodName.empty() ||
            d.message.find("`" + methodName + "`") != std::string::npos)
            return true;
    }
    return false;
}

}  // namespace

// --- Positive: the exact §28.3 / #1499 "Make(): Res" shape gets the nudge. ---

BEZEL_TEST(static_factory_owned_nudge_1600, sourceless_static_factory_warns) {
    auto r = parseAndCheckOwnership(
        "module M; class Res "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Res; "
        "class Factory "
        "  public static method Make(): Res return new Res(); end Make; "
        "  public static method Main(): int "
        "    var m := Make(); "
        "    return 0; "
        "  end Main; "
        "end Factory;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasFactoryNudgeWarning(r, "Make"));
}

// The nudge is a DECLARATION-level lint: it fires even though the fresh
// return in `Make` is discharged with an explicit `using var` at every call
// site — the diagnostic is about the callee's ambiguous signature, not about
// any particular call site's discharge behavior.
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
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasFactoryNudgeWarning(r, "Make"));
}

// Expression-bodied static factories (`Make(): Res => new Res();`) get the
// same nudge — staticFactoryReturnsFreshOwned handles both bodyKinds.
BEZEL_TEST(static_factory_owned_nudge_1600,
           expression_bodied_sourceless_factory_warns) {
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
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasFactoryNudgeWarning(r, "Make"));
}

// --- Negative: already-annotated `owned` return needs no nudge. ---

BEZEL_TEST(static_factory_owned_nudge_1600, owned_return_no_nudge) {
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
    BEZEL_EXPECT_FALSE(hasFactoryNudgeWarning(r, "Make"));
}

// --- Negative: an INSTANCE method has a structural source (the receiver) —
// §28.3 producers table ("instance method: source is the receiver"). ---

BEZEL_TEST(static_factory_owned_nudge_1600, instance_method_no_nudge) {
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
    BEZEL_EXPECT_FALSE(hasFactoryNudgeWarning(r, "Get"));
}

// --- Negative: a value-typed return carries no ownership obligation at all —
// staticFactoryReturnsFreshOwned's plain-class/interface guard excludes it.
// ---

BEZEL_TEST(static_factory_owned_nudge_1600, value_typed_return_no_nudge) {
    auto r = parseAndCheckOwnership(
        "module M; class Factory "
        "  public static method Make(): int return 5; end Make; "
        "  public static method Main(): int "
        "    var m := Make(); "
        "    return 0; "
        "  end Main; "
        "end Factory;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasFactoryNudgeWarning(r, "Make"));
}

// --- v0.196 RETIREMENT (#1815): a static method returning a BORROW (a static-
// field read) is now REJECTED (§28.3 ~L5898 / §28.6 ~L6085 — the static-method
// static-storage re-exposure arm is retired; re-exposure is the static property
// getter only). This shape previously pinned the now-retired acceptance (no
// error, no nudge); flipping the error expectation IS the spec-mandated behavior
// change. The nudge assertion still holds: the nudge fires only for a
// fresh-owned return (`staticFactoryReturnsFreshOwned`), and a static-field read
// is a borrow, not fresh — so there is still nothing to annotate `owned` and no
// nudge fires. The borrow-return Error, not a nudge, is now what is diagnosed. ---

BEZEL_TEST(static_factory_owned_nudge_1600, borrow_field_return_no_nudge) {
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
    BEZEL_EXPECT_FALSE(hasFactoryNudgeWarning(r, "Get"));
}

// --- Negative: iterator methods are explicitly exempt (§28.3: "adding
// `owned` to every iterator return type would be redundant noise"). ---

BEZEL_TEST(static_factory_owned_nudge_1600, iterator_method_no_nudge) {
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
    BEZEL_EXPECT_FALSE(hasFactoryNudgeWarning(r, "Make"));
}

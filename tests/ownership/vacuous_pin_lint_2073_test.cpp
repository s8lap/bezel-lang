// Spec-derived regression for issue #2073: vacuous-pin lint diagnostic. The
// `using var` keyword admits every RHS shape uniformly (§28.10: "safe to use
// uniformly in generic code … without inspecting the underlying type kind"),
// but two cases make the keyword strictly less expressive than `const`:
//   1. Inert RHS — a value-typed or runtime-managed value carries no disposal
//      obligation (§28.9), so the scope-end `Dispose()` a `using var` would
//      call is a no-op; only the reassignment lock survives.
//   2. Vacuous borrow-pin — a borrow-pin whose source already outlives the
//      enclosing scope (parameter, `this`, static field, or a field chain
//      rooted in such). The pin extends nothing.
// Both cases fire a warning steering the programmer toward `const x := …;`
// (§21.1, v0.201) — the read-only alias with no disposal hint. NEVER an
// error; suppressed in generic bodies where the classification varies per
// instantiation (§28.10's reason for the uniform admissibility).
//
// Spec §28.10 (v0.215):
//   "`using var` admits a value-type or runtime-managed right-hand side. For
//    ordinary value types and runtime-managed types, the disposal at scope
//    end is a no-op (these types do not require disposal per §28.9), and the
//    only effect is the reassignability restriction."
// Rationale §17.8 (v0.201): "`using var` over a borrow/value is an admitted
//    readonly spelling — the pre-existing spelling stops being needed."
//
// Rework (2026-07-22): a method is "generic-bodied" for lint suppression
// when EITHER the method declares its own `<T>` parameters OR the enclosing
// type is generic (`class C<T> method M(c: T) using var x := c`). The
// rework's original-commit `!md->genericParams.empty()` check missed the
// latter — a non-generic method inside a generic class was incorrectly
// fired. Negative-control tests below pin both flavors.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// True iff some WARNING diagnostic fires the inert-RHS vacuous-pin message.
bool hasInertRhsWarning(const OwnershipResult& r, const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("is vacuous") == std::string::npos) continue;
        if (d.message.find("RHS is a value-typed/runtime-managed") ==
            std::string::npos)
            continue;
        if (d.message.find("`" + name + "`") == std::string::npos) continue;
        return true;
    }
    return false;
}

// True iff some WARNING diagnostic fires the vacuous-borrow-pin message.
bool hasVacuousBorrowPinWarning(const OwnershipResult& r,
                                const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("pins a source that already outlives") ==
            std::string::npos)
            continue;
        if (d.message.find("`" + name + "`") == std::string::npos) continue;
        return true;
    }
    return false;
}

// Counts how many times the inert-RHS warning fires for `name`. Mirrors the
// per-declaration emission contract — exactly one per vacuous declaration.
int countInertRhsWarnings(const OwnershipResult& r, const std::string& name) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("is vacuous") == std::string::npos) continue;
        if (d.message.find("`" + name + "`") == std::string::npos) continue;
        ++n;
    }
    return n;
}

// Counts how many times the vacuous-borrow-pin warning fires for `name`.
int countVacuousBorrowPinWarnings(const OwnershipResult& r,
                                  const std::string& name) {
    int n = 0;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("pins a source that already outlives") ==
            std::string::npos)
            continue;
        if (d.message.find("`" + name + "`") == std::string::npos) continue;
        ++n;
    }
    return n;
}

// `using var` over a value-typed parameter: §28.9 says value types carry no
// disposal obligation, so the using-var's disposal at scope end is a no-op and
// only the reassignment lock survives — `const` states it better.
BEZEL_TEST(vacuous_pin_lint_2073,
           inert_int_param_steers_to_const_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public method M(b: int): int "
        "    using var x := b; "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInertRhsWarning(r, "x"));
    BEZEL_EXPECT_EQ(countInertRhsWarnings(r, "x"), 1);
}

// `using var` over a string literal: runtime-managed (§28.9) → no disposal.
BEZEL_TEST(vacuous_pin_lint_2073,
           inert_string_literal_steers_to_const_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public method M(): int "
        "    using var s := \"hello\"; "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInertRhsWarning(r, "s"));
    BEZEL_EXPECT_EQ(countInertRhsWarnings(r, "s"), 1);
}

// `using var` over a class-typed borrow parameter: pin extends nothing
// because the parameter already outlives this scope. Spec §28.10 / §28.3.
BEZEL_TEST(vacuous_pin_lint_2073,
           borrow_param_pin_is_vacuous_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell public constructor() end constructor; "
        "destructor end destructor; end Cell; "
        "class C "
        "  public method M(c: Cell): int "
        "    using var x := c; "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasVacuousBorrowPinWarning(r, "x"));
    BEZEL_EXPECT_EQ(countVacuousBorrowPinWarnings(r, "x"), 1);
}

// `using var` over `this.field`: receiver outlives the scope, so the field
// read's lifetime is already covered by the receiver's. Pin extends nothing.
BEZEL_TEST(vacuous_pin_lint_2073,
           this_field_pin_is_vacuous_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell public constructor() end constructor; "
        "destructor end destructor; end Cell; "
        "class C "
        "  private owned _cell: Cell := new Cell(); "
        "  public method M(): int "
        "    using var x := this._cell; "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasVacuousBorrowPinWarning(r, "x"));
    BEZEL_EXPECT_EQ(countVacuousBorrowPinWarnings(r, "x"), 1);
}

// Negative control — `using var` over a fresh owned producer IS load-bearing:
// the using-var adopts the disposal obligation. No warning (positive control).
BEZEL_TEST(vacuous_pin_lint_2073,
           fresh_owned_producer_load_bearing_no_warn_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell public constructor() end constructor; "
        "destructor end destructor; end Cell; "
        "class C "
        "  public method M(): int "
        "    using var c := new Cell(); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "c"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "c"));
}

// Negative control — `using var y := owned x` (marked transfer) is a §28.3
// binding-position adoption: load-bearing, no warning. The marker filter
// (`!ld.initializerHasOwnedMarker`) excludes the adoption shape.
BEZEL_TEST(vacuous_pin_lint_2073,
           marked_owned_transfer_load_bearing_no_warn_spec_28_3) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell public constructor() end constructor; "
        "destructor end destructor; end Cell; "
        "class C "
        "  public method M(owned c: Cell): int "
        "    using var local := owned c; "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "local"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "local"));
}

// Negative control — `using var` over a scope-local borrow: the pin EXTENDS
// the local's lifetime past its natural death. Load-bearing — no warning.
// The rhsIsVacuousBorrowPin helper returns false for LocalSymbol (locals
// never outlive their scope).
BEZEL_TEST(vacuous_pin_lint_2073,
           borrow_local_pin_load_bearing_no_warn_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell public constructor() end constructor; "
        "destructor end destructor; end Cell; "
        "class C "
        "  public method M(c: Cell): int "
        "    var borrow: Cell := c; "
        "    using var pin := borrow; "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "pin"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "pin"));
}

// Negative control — generic method body suppresses BOTH warnings: the
// classification varies per instantiation (§28.10's reason for the uniform
// admissibility), so a value-typed RHS at one instantiation is class-typed
// at another and the lint cannot steer without false positives.
// // per §28.10 #2073
BEZEL_TEST(vacuous_pin_lint_2073,
           generic_method_body_suppresses_lint_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public method M<T>(t: T): int "
        "    using var x := t; "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "x"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "x"));
}

// Negative control (rework 2026-07-22) — a non-generic method INSIDE a generic
// class MUST also suppress the lint: the classification of `t: T` varies per
// instantiation (§28.10), so the same source-program can see an inert RHS at
// `C<int>` and a load-bearing one at `C<Cell>` — steering is unsound without
// the per-instantiation information. The original `e61b032` only checked the
// method-level `!md->genericParams.empty()` and missed this case (Opus
// review catch). FAILS WITHOUT THE REWORK.
// // per §28.10 #2073 (rework)
BEZEL_TEST(vacuous_pin_lint_2073,
           generic_class_body_suppresses_lint_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C<T> "
        "  public method M(c: T): int "
        "    using var x := c; "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "x"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "x"));
}

// Negative control (rework 2, 2026-07-22) — `using var` over a disposable
// class type is the CORRECT canonical pattern (§28.10), not vacuous: the
// scope-end `Dispose()` runs real cleanup logic. The original implementation
// keyed only on `isBorrowableType`, which returns false for built-in class
// heads (Channel, EventSource, OwningArray, …) because they have no user
// TypeDecl — so the named-class branch falls through to the
// not-a-builtin-exception path and reports false. Without the rework-2
// exclusion the inert-RHS check over-fires on every disposable-typed
// `using var` binding (the #600 Channel/EventSource/etc. acceptance tests
// and the #1595 `using var t := F.Work(p)` Task-family pattern). FAILS WITHOUT
// THE REWORK. // per §28.10 #2073 (rework 2)

// §28.9 (v0.240), fix #2269: StringBuilder is TRIVIAL-DISPOSE — the spec
// (L7363) names it explicitly ("declares no destructor and no `owned`
// fields, and its intrinsic backing storage is released by the instance's
// own free, exactly as a borrowing collection's is"), and §28.9 L7357
// forbids the ownership-discharge warning on a `new` expression of a
// trivial-dispose class type whose result is not stored in `owned` or
// `using var`. The scope-end reclaim still runs (the codegen auto-drop
// chain reclaims the builder via the borrowing-`Array<string>` free path,
// #1185 / LlvmBackend_Setup.cpp:15725) but it is the §28.9
// trivial-dispose reclaim, not the must-use discharge; the warning the
// `#2190` test pinned was based on a stale v0.226 reading and is wrong
// at v0.240. The `using var` half of the regression (no inert-RHS / no
// vacuous-pin warning) is unchanged from #2190: it was vacuous the whole
// time — StringBuilder was never in `kDisposableBuiltinHeadNames`'s
// neighbour table `kChainClaimedBuiltinHeadNames` either, so the lint
// also correctly skipped. // per §28.9 (v0.240), fix #2269
BEZEL_TEST(stringbuilder_ownership_hint_2269,
           stringbuilder_trivial_dispose_no_ceremony_warning_v0240) {
    auto usingResult = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public method Using(): int "
        "    using var sb := new StringBuilder(); "
        "    return sb.Length; "
        "  end Using; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(usingResult.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(usingResult, "sb"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(usingResult, "sb"));

    auto plainResult = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public method Plain(): int "
        "    var sb := new StringBuilder(); "
        "    return sb.Length; "
        "  end Plain; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    // §28.9 L7357: a `new` of a trivial-dispose class type whose result is
    // not stored in `owned` or `using var` does NOT produce the standard
    // ownership-discharge warning. The scope-end reclaim still runs (the
    // codegen chain reclaims the builder's backing array via the
    // borrowing-`Array<string>` free path) but it is the trivial-dispose
    // reclaim, not a must-use discharge.
    BEZEL_EXPECT_FALSE(plainResult.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(bezel::test::hasAutoDropWarning(plainResult, "sb"));
}

// `using var` over a fresh Channel<T> — the §6.9 / #600 canonical pattern.
BEZEL_TEST(vacuous_pin_lint_2073,
           channel_using_var_no_warn_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "record Report(Name: string) end Report; "
        "class C "
        "  public method M(): int "
        "    using var ch := new Channel<Report>(); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "ch"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "ch"));
}

// `using var` over a ChannelSender<T> (the §6.9 send endpoint).
BEZEL_TEST(vacuous_pin_lint_2073,
           channel_sender_using_var_no_warn_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "record Report(Name: string) end Report; "
        "class C "
        "  public method M(): int "
        "    using var inbox := new Channel<Report>(); "
        "    using var sender := inbox.NewSender(); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "inbox"));
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "sender"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "sender"));
}

// `using var` over a fresh EventSource<T> — the §6.5 / #1536 canonical
// pattern (destructor severs the subscriber list before reclamation,
// §28.14 quiescence).
BEZEL_TEST(vacuous_pin_lint_2073,
           event_source_using_var_no_warn_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public method M(): int "
        "    using var src := new EventSource<int>(); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "src"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "src"));
}

// `using var inbox := ch;` where `ch` is a plain-`var`-owned Channel local
// (the borrow-pin classifier returns false for locals — the pin extends
// the local's lifetime, which IS load-bearing for an owned local — and
// the inert-RHS branch must also not fire because `Channel<Report>` is a
// disposable class). // per §28.10 #2073 (rework 2)
BEZEL_TEST(vacuous_pin_lint_2073,
           local_channel_borrow_using_var_no_warn_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "record Report(Name: string) end Report; "
        "class C "
        "  public method M(): int "
        "    var ch := new Channel<Report>(); "
        "    using var inbox := ch; "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "inbox"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "inbox"));
}

// `using var` over a Task-family return (§28.3 "calls mint" / v0.190 SB-01) —
// the canonical pattern for binding a task frame whose disposal obligation
// transfers to the caller. FAILS WITHOUT THE REWORK (the `Task<Pair>` type
// is a built-in with no user TypeDecl, so isBorrowableType returns false
// and the inert-RHS branch over-fires; see the #1595 acceptance test).
// // per §28.10 #2073 (rework 2)
BEZEL_TEST(vacuous_pin_lint_2073,
           task_using_var_no_warn_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "record Pair(X: int, Y: int); "
        "static class F public static async method Work<T>(x: T): Task<T> return x; end Work; end F; "
        "class C "
        "  public static method Main(): int "
        "    var p := new Pair(1, 2); "
        "    using var t := F.Work(p); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "t"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "t"));
}

// `using var` over an `owning` collection (OwningArray<T>) — the §4
// canonical pattern (Dispose cascades through every element).
// // per §28.10 #2073 (rework 2)
BEZEL_TEST(vacuous_pin_lint_2073,
           owning_array_using_var_no_warn_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell public constructor() end constructor; "
        "destructor end destructor; end Cell; "
        "class C "
        "  public method M(): int "
        "    using var arr := new OwningArray<Cell>(); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "arr"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "arr"));
}

// Sanity control — the rework-2 exclusion must NOT mask true inert-RHS
// diagnostics on value-typed / runtime-managed types: `using var x := 42`
// is still vacuous and STILL fires the inert-RHS warning (the int RHS
// is not a disposable class type). // per §28.10 #2073 (rework 2)
BEZEL_TEST(vacuous_pin_lint_2073,
           rework2_does_not_mask_int_param_lint) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public method M(b: int): int "
        "    using var x := b; "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInertRhsWarning(r, "x"));
    BEZEL_EXPECT_EQ(countInertRhsWarnings(r, "x"), 1);
}

// Sanity control — `using var s := "hello"` (runtime-managed string)
// remains inert and STILL fires the inert-RHS warning.
// // per §28.10 #2073 (rework 2)
BEZEL_TEST(vacuous_pin_lint_2073,
           rework2_does_not_mask_string_literal_lint) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public method M(): int "
        "    using var s := \"hello\"; "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInertRhsWarning(r, "s"));
    BEZEL_EXPECT_EQ(countInertRhsWarnings(r, "s"), 1);
}

// ============================================================================
// rework 3c — direct negative-control coverage for EVERY retained head in
// `rhsIsDisposableClassType` (#2073). Each test below exercises one head: a
// `using var x := new <Head>()`, asserting the inert-RHS warning does NOT
// fire and the borrow-pin warning does NOT fire. The heads without public
// constructors (`Subscription`, `SemaphoreToken`, `Lock`, `OwningTask`,
// `BinaryDeserializer`, `JsonDeserializer`) are tested via the public API
// that produces them — the head is the static type of the `using var`'s
// RHS, which is what `rhsIsDisposableClassType` keys on.
// // per §28.10 / §28.11, #2073 (rework 3c)
// ============================================================================

// AsyncEventSource<T> — §6.5 / #1536 canonical pattern (async variant of
// EventSource). Destructor severs the subscriber list before reclamation.
BEZEL_TEST(vacuous_pin_lint_2073,
           async_event_source_using_var_no_warn_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; import System.Serialization; "
        "class C "
        "  public method M(): int "
        "    using var src := new AsyncEventSource<int>(); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "src"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "src"));
}

// Subscription — no public constructor; obtained from `EventSource.Subscribe`
// (or analogous register-and-unregister APIs, §2.11 stdlib). The
// `using var` over the returned `Subscription` IS the canonical pattern.
// // per §28.10 / §2.11, #2073 (rework 3c)
BEZEL_TEST(vacuous_pin_lint_2073,
           subscription_using_var_no_warn_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; import System.Serialization; "
        "class C "
        "  public method M(): int "
        "    using var src := new EventSource<int>(); "
        "    using var sub := src.Subscribe(lambda(x) => do return (); end); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "sub"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "sub"));
}

// TaskScope — §6.10 / #1990 (IAsyncDisposable; `await using` only — the
// sync `using var` form is rejected at the binding site, §6.10 v0.175). The
// load-bearing assertion is that the binding does NOT also fire the
// inert-RHS warning: `TaskScope` is on the disposable-built-in list and the
// `rhsIsDisposableClassType` exclusion fires before any §6.10 complaint.
// // per §28.10 / §6.10, #2073 (rework 3c)
BEZEL_TEST(vacuous_pin_lint_2073,
           task_scope_using_var_no_warn_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public async method M(): Task<int> "
        "    await using var scope := new TaskScope(); "
        "    await scope.Join(); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "scope"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "scope"));
}

// CancellationTokenSource — §6.1 (auto-imported via System).
BEZEL_TEST(vacuous_pin_lint_2073,
           cancellation_token_source_using_var_no_warn_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public method M(): int "
        "    using var cts := new CancellationTokenSource(); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "cts"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "cts"));
}

// Mutex — §6.2 (synchronized primitive; `Shared`-payload sendable set).
BEZEL_TEST(vacuous_pin_lint_2073,
           mutex_using_var_no_warn_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public method M(): int "
        "    using var m := new Mutex(); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "m"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "m"));
}

// Semaphore — §6.2 (synchronized primitive; constructor requires
// `initialCount: int`). The Mutex/Semaphore heads are listed alongside the
// §6.2 synchronized primitives — both have non-trivial `Dispose` (release
// internal native handle).
BEZEL_TEST(vacuous_pin_lint_2073,
           semaphore_using_var_no_warn_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public method M(): int "
        "    using var sem := new Semaphore(1); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "sem"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "sem"));
}

// SemaphoreToken — no public constructor; obtained from Semaphore.Acquire.
// The `using var` over the returned token IS the canonical §6.2 pattern.
// // per §28.10 / §6.2, #2073 (rework 3c)
BEZEL_TEST(vacuous_pin_lint_2073,
           semaphore_token_using_var_no_warn_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public method M(): int "
        "    using var sem := new Semaphore(1); "
        "    using var tok := sem.Acquire(); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "tok"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "tok"));
}

// Lock — no public constructor; obtained from Mutex.Acquire (the §6.2
// canonical pattern). The `using var` over the held lock is the documented
// canonical pattern in the §6.2 example.
// // per §28.10 / §6.2, #2073 (rework 3c)
BEZEL_TEST(vacuous_pin_lint_2073,
           lock_using_var_no_warn_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public method M(): int "
        "    using var m := new Mutex(); "
        "    using var guard := m.Acquire(); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "guard"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "guard"));
}

// Shared<T> — §2.16 / §28.13 (scoped shared ownership; `IDisposable`,
// last-disposer-wins). The constructor consumes an owned value; uses a
// user-defined class so the disposal is observable in the test surface.
BEZEL_TEST(vacuous_pin_lint_2073,
           shared_using_var_no_warn_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell public constructor() end constructor; "
        "destructor end destructor; end Cell; "
        "class C "
        "  public method M(): int "
        "    using var cell := new Shared<Cell>(new Cell()); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "cell"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "cell"));
}

// Weak<T> — §2.16 (no public constructor; obtained from `Shared.Downgrade()`).
// The `using var` over the `Weak` observer releases the weak handle at
// scope end; the underlying value is unaffected.
// // per §28.10 / §2.16, #2073 (rework 3c)
BEZEL_TEST(vacuous_pin_lint_2073,
           weak_using_var_no_warn_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell public constructor() end constructor; "
        "destructor end destructor; end Cell; "
        "class C "
        "  public method M(): int "
        "    using var sh := new Shared<Cell>(new Cell()); "
        "    using var w := sh.Downgrade(); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "w"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "w"));
}

// OwningDictionary<K, V> — §4.9 (Dispose cascades over values). The
// `where V: class` constraint requires a class-typed value; use a user class
// so the disposal cascade is observable.
BEZEL_TEST(vacuous_pin_lint_2073,
           owning_dictionary_using_var_no_warn_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell public constructor() end constructor; "
        "destructor end destructor; end Cell; "
        "class C "
        "  public method M(): int "
        "    using var d := new OwningDictionary<string, Cell>(); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "d"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "d"));
}

// OwningSet<T> — §4.10 (Dispose cascades over elements).
BEZEL_TEST(vacuous_pin_lint_2073,
           owning_set_using_var_no_warn_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell public constructor() end constructor; "
        "destructor end destructor; end Cell; "
        "class C "
        "  public method M(): int "
        "    using var s := new OwningSet<Cell>(); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "s"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "s"));
}

// OwningStack<T> — §4.11 (Dispose cascades over elements).
BEZEL_TEST(vacuous_pin_lint_2073,
           owning_stack_using_var_no_warn_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell public constructor() end constructor; "
        "destructor end destructor; end Cell; "
        "class C "
        "  public method M(): int "
        "    using var stk := new OwningStack<Cell>(); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "stk"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "stk"));
}

// OwningQueue<T> — §4.12 (Dispose cascades over elements).
BEZEL_TEST(vacuous_pin_lint_2073,
           owning_queue_using_var_no_warn_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell public constructor() end constructor; "
        "destructor end destructor; end Cell; "
        "class C "
        "  public method M(): int "
        "    using var q := new OwningQueue<Cell>(); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "q"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "q"));
}

// OwningLinkedList<T> — §4.7 (Dispose cascades over nodes/elements).
BEZEL_TEST(vacuous_pin_lint_2073,
           owning_linked_list_using_var_no_warn_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell public constructor() end constructor; "
        "destructor end destructor; end Cell; "
        "class C "
        "  public method M(): int "
        "    using var list := new OwningLinkedList<Cell>(); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "list"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "list"));
}

// OwningBuffer<T> — §4.14 (the storage primitive over which resizable
// owning collections compose; class-T form disposes each non-null slot).
BEZEL_TEST(vacuous_pin_lint_2073,
           owning_buffer_using_var_no_warn_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell public constructor() end constructor; "
        "destructor end destructor; end Cell; "
        "class C "
        "  public method M(): int "
        "    using var buf := new OwningBuffer<Cell>(4); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "buf"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "buf"));
}

// TaskCompletionSource (non-generic, void-completion form) — §6.6.
BEZEL_TEST(vacuous_pin_lint_2073,
           task_completion_source_using_var_no_warn_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public method M(): int "
        "    using var tcs := new TaskCompletionSource(); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "tcs"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "tcs"));
}

// OwningTask — §6.7 negative control. `Task.FromOwnedResult<T>(owned value: T):
// OwningTask<T> where T: class` (stdlib §6.7) returns an OwningTask<T> — the
// `OwningTask` head is in rhsIsDisposableClassType's disposable-builtin set
// (spec §28.10 / §28.11), so the inert-RHS branch must NOT fire; this pins
// that the head-name exclusion fires for the call-form, not only the new-form.
// Per spec §28.10 (v0.215) + §30.3; the owning-consume context is `using var`.
BEZEL_TEST(vacuous_pin_lint_2073, owning_task_using_var_no_warn_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell public constructor() end constructor; "
        "destructor end destructor; end Cell; "
        "class C "
        "  public method M(): int "
        "    var cell := new Cell(); "
        "    using var t := Task.FromOwnedResult<Cell>(cell); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "t"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "t"));
}

// OwningTaskCompletionSource<T> — §6.6 (class-T form: Pattern B task).
BEZEL_TEST(vacuous_pin_lint_2073,
           owning_task_completion_source_using_var_no_warn_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell public constructor() end constructor; "
        "destructor end destructor; end Cell; "
        "class C "
        "  public method M(): int "
        "    using var tcs := new OwningTaskCompletionSource<Cell>(); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "tcs"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "tcs"));
}

// BinarySerializer — §2.15 concrete format (implements IDisposable, owns
// internal growable byte buffer). The §2.15 docstring example uses exactly
// this `using var s := new BinarySerializer();` pattern. Note: the type
// lives in `System.Serialization` (NOT auto-imported); the in-process
// helper has no module loader, so the type-resolution diagnostic is
// tolerated. The load-bearing assertion is the inert-RHS warning is NOT
// emitted — `rhsIsDisposableClassType` recognizes the head from
// `headName()` (which is the unparsed textual name), so the head-name
// exclusion fires before any type resolution would fail.
// // per §28.10 / §2.15, #2073 (rework 3c)
BEZEL_TEST(vacuous_pin_lint_2073,
           binary_serializer_using_var_no_warn_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; import System.Serialization; "
        "class C "
        "  public method M(): int "
        "    using var bs := new BinarySerializer(); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "bs"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "bs"));
}

// JsonSerializer — §2.15 (mirror of BinarySerializer).
BEZEL_TEST(vacuous_pin_lint_2073,
           json_serializer_using_var_no_warn_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; import System.Serialization; "
        "class C "
        "  public method M(): int "
        "    using var js := new JsonSerializer(); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "js"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "js"));
}

// BinaryDeserializer — §2.15 (constructor takes a `ReadOnlySpan<byte>`;
// using `new BinaryDeserializer(<span>)` mirrors the §2.15 docstring
// example for `LoadPoint`). Tolerated: type-resolution diagnostic from the
// un-imported serializer module; load-bearing: the inert-RHS warning is
// NOT emitted.
// // per §28.10 / §2.15, #2073 (rework 3c)
BEZEL_TEST(vacuous_pin_lint_2073,
           binary_deserializer_using_var_no_warn_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public method M(): int "
        "    using var buf := new OwningBuffer<byte>(1); "
        "    using var d := new BinaryDeserializer(buf.AsReadOnlySpan()); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "d"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "d"));
}

// JsonDeserializer — §2.15 (mirror of BinaryDeserializer; constructor takes
// a `ReadOnlySpan<byte>` input — same `AsReadOnlySpan()` plumbing).
// // per §28.10 / §2.15, #2073 (rework 3c)
BEZEL_TEST(vacuous_pin_lint_2073,
           json_deserializer_using_var_no_warn_spec_28_10) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public method M(): int "
        "    using var buf := new OwningBuffer<byte>(1); "
        "    using var d := new JsonDeserializer(buf.AsReadOnlySpan()); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "d"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "d"));
}

// ============================================================================
// rework 3c — positive controls for the borrowing-collection TRIVIAL-DISPOSE
// carve-out. Per §28.11 (v0.240) the borrowing family (Array/Stack/Queue/Set/
// SortedSet/Dictionary/SortedDictionary/LinkedList) does "no user-visible
// work" in `Dispose()`, so pinning one with `using var` states a disposal
// obligation the type does not have — the §21.1 `const` alias is the correct
// spelling. These tests pin the §28.10/§28.11 narrowing of the disposable-
// types list: a future rework that adds borrowing collections to
// `rhsIsDisposableClassType` (a regression to the pre-v0.215 surface) would
// silence these inert warnings, breaking the test — making the carve-out
// structural, not just commented.
//
// ⚠ AMENDED for #2695 (v0.240): the RHS is now a READ of an already-bound
// collection local, not a `new` expression. The original form
// (`using var a := new Array<int>();`) asserted a diagnostic whose own remedy
// the compiler REJECTS — §21.1 refuses `const a := new Array<int>();` ("a
// `const` local (`a`) must not take an owned production"), so the advice was
// unfollowable and the shape had NO warning-free spelling. #2695 scopes the
// lint to RHSs whose `const` remedy compiles. Reading a bound local keeps the
// RHS's static type exactly the borrowing-collection type this carve-out is
// about — `rhsIsDisposableClassType` sees the identical `TypeRef`, so the
// regression this test was built to catch is caught verbatim — while the
// remedy (`const p := a;`) is legal, which
// `vacuous_lint_const_remedy_2695_test.cpp` asserts directly.
// // per §28.10 / §28.11 / §21.1, #2073 (rework 3c), #2695
// ============================================================================
BEZEL_TEST(vacuous_pin_lint_2073,
           borrowing_collections_inert_rhs_warns_spec_28_11) {
    const char* cases[] = {
        "Array<int>", "Stack<int>", "Queue<int>", "Set<int>",
        "SortedSet<int>", "Dictionary<string, int>",
        "SortedDictionary<string, int>", "LinkedList<int>"};
    const char* names[] = {"a", "s", "q", "set", "sorted", "dict",
                           "sortedDict", "list"};
    for (std::size_t i = 0; i < std::size(cases); ++i) {
        // `src` is the owned binding; `names[i]` is the vacuous pin over it.
        std::string source =
            std::string("module M; class C public method M(): int var src := new ") +
            cases[i] + "(); using var " + names[i] + " := src; return 0; end M; "
            "public static method Main(): int return 0; end Main; end C;";
        auto r = parseAndCheckOwnership(source);
        BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
        BEZEL_EXPECT_TRUE(hasInertRhsWarning(r, names[i]));
    }
}

// ============================================================================
// rework 4 (#2385, #2451) — the iteration-protocol sentinel family
// (IEnumerable<T>/IEnumerator<T>/IOwningEnumerable<T>/IOwningEnumerator<T>
// and the async counterparts) is declaration-less (TypeResolution_Names.cpp:
// "declaration stays null (no user TypeDecl)"), same representation as the
// Channel<T>/EventSource<T> builtins above — so isBorrowableType's
// null-declaration fallthrough classified every one of them "not borrowable",
// and before this rework `rhsIsDisposableClassType`'s Path-2 allow-list did
// not recognize the family either, so Case A's inert-RHS guard wrongly fired.
// Spec §29.1 (v0.235): "`GetEnumerator()` returns `owned IEnumerator<T>`: the
// caller takes ownership of the enumerator and must dispose it" —
// unconditional, independent of whether the implementation is a compiler-
// generated `yield return` iterator (#2385) or a hand-written two-class
// implementation reached through `wrapUserClassAsIEnumerable` (#2451).
// FAILS WITHOUT THE REWORK (mechanically confirmed: reverting just the
// `rhsIsDisposableClassType` head-name arm reproduces the inert-RHS warning
// on every case below).
// // per §28.10, §29.1, stdlib §4.12/§4.13, #2385, #2451
// ============================================================================

// #2385 repro (compiler-generated iterator): `src.GetEnumerator()` on a
// named `IEnumerable<int>` local backed by a `yield return` iterator method.
// The result type is the declaration-less `IEnumerator<int>` sentinel; a
// `using var` over it is the canonical §29.1 pattern, not vacuous.
BEZEL_TEST(vacuous_pin_lint_2073,
           manual_getenumerator_compiler_iterator_no_warn_spec_29_1_2385) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method Gen(): IEnumerable<int> "
        "    yield return 1; yield return 2; yield return 3; "
        "  end Gen; "
        "  public static method M(): int "
        "    var src := C.Gen(); "
        "    using var e1 := src.GetEnumerator(); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "e1"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "e1"));
}

// #2451 repro (hand-written two-class implementation, wrapUserClassAsIEnumerable):
// a manual `.GetEnumerator()` call on a user class implementing `IEnumerable<T>`
// returns a genuine, freshly-`calloc`'d wrapper with its own `Dispose()`
// (hardened by #2389/#2424/#2440); `using var` over it must not warn.
BEZEL_TEST(vacuous_pin_lint_2073,
           manual_getenumerator_user_class_no_warn_spec_29_1_2451) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class IntRangeEnum: IEnumerator<int> "
        "  private lo: int; private hi: int; private cur: int; "
        "  public constructor(lo: int, hi: int) "
        "    this.lo:=lo; this.hi:=hi; cur:=lo-1; "
        "  end constructor; "
        "  public method MoveNext(): bool cur:=cur+1; return cur<=hi; end MoveNext; "
        "  public property Current: int get => cur; end Current; "
        "end IntRangeEnum; "
        "class IntRange: IEnumerable<int> "
        "  private lo: int; private hi: int; "
        "  public constructor(lo: int, hi: int) this.lo:=lo; this.hi:=hi; end constructor; "
        "  public method GetEnumerator(): IEnumerator<int> "
        "    return new IntRangeEnum(lo, hi); "
        "  end GetEnumerator; "
        "end IntRange; "
        "class C "
        "  public static method M(): int "
        "    using var s := new IntRange(1, 3); "
        "    using var e1 := s.GetEnumerator(); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "e1"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "e1"));
}

// #2451's broader claim: the misfire is not specific to a `GetEnumerator()`
// call result — ANY `using var` whose static RHS type is the sentinel family
// is affected, e.g. an `Array<T>` coerced to `IEnumerable<T>`. Coercing a
// borrowing collection into the interface allocates a fresh vtable-backed
// wrapper (codegen's `wrappedHere`/`isSyncIterEnum` path, LlvmBackend_Setup.cpp)
// that genuinely owns disposal of the wrapper frame (it borrows the array —
// exactly-once, does not double-free it).
BEZEL_TEST(vacuous_pin_lint_2073,
           array_wrapped_ienumerable_local_no_warn_spec_29_1_2451) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method M(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(1); a.Add(2); "
        "    using var seq: IEnumerable<int> := a; "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "seq"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "seq"));
}

// IOwningEnumerator<T> — stdlib §4.13 (`: IDisposable where T: class`), reached
// via `OwningArray<T>.Drain().GetOwningEnumerator()`. Same sentinel shape,
// owning-family counterpart of the borrowing `IEnumerator<T>` case above.
BEZEL_TEST(vacuous_pin_lint_2073,
           owning_enumerator_using_var_no_warn_spec_29_1_2451) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell public constructor() end constructor; "
        "destructor end destructor; end Cell; "
        "class C "
        "  public static method M(): int "
        "    using var arr := new OwningArray<Cell>(); "
        "    arr.Add(new Cell()); "
        "    using var d := arr.Drain(); "
        "    using var e := d.GetOwningEnumerator(); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "e"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "e"));
}

// Positive-direction proof: excluding the sentinel family from Case A
// (inert-RHS) does not silence the lint for it entirely — Case B (vacuous
// borrow-pin) becomes REACHABLE for it instead, and correctly fires when the
// specific RHS genuinely is a vacuous pin. `this._seq` is a FIELD READ
// (§28.10 "using var initialized from a borrow": "a method call without an
// owned return … an indexer access, a field read, or an unmarked read of a
// named owned binding" is a borrow), and its root (`this`) already outlives
// this method's scope, so the pin extends nothing — the SAME diagnosis Case B
// gives for an ordinary declared-class field (this_field_pin_is_vacuous_
// spec_28_10 above). Before this rework this shape was swallowed by Case A's
// wrong "inert" message instead of Case B's correct "pins a source that
// already outlives this scope" one; mechanically confirmed via
// `agent_check.sh --probe` that Case B's message fires post-fix.
BEZEL_TEST(vacuous_pin_lint_2073,
           owned_field_of_sentinel_type_borrow_pin_still_warns_spec_28_10_2451) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  private owned _seq: IEnumerable<int> := C.Gen(); "
        "  public static method Gen(): IEnumerable<int> "
        "    yield return 1; yield return 2; "
        "  end Gen; "
        "  public method N(): int "
        "    using var x := this._seq; "
        "    return 0; "
        "  end N; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "x"));
    BEZEL_EXPECT_TRUE(hasVacuousBorrowPinWarning(r, "x"));
    BEZEL_EXPECT_EQ(countVacuousBorrowPinWarnings(r, "x"), 1);
}

// Positive-direction proof: the rework must NOT mask a genuinely inert RHS.
// `IList<int>` is a DIFFERENT declaration-less sentinel family (the
// array-backed, unwrapped protocol per iterator_native_test.cpp's
// `ilist_local_unwrapped_control`) that is deliberately excluded from
// `rhsIsDisposableClassType` — only the eight iteration-protocol names were
// added. A `using var` pinning a borrowing `Array<int>` through `IList<int>`
// remains genuinely vacuous (no fresh wrapper is allocated for IList) and
// must still warn, proving the fix is scoped to the exact head-name set, not
// a blanket "any interface" carve-out.
BEZEL_TEST(vacuous_pin_lint_2073,
           ilist_sentinel_still_warns_spec_28_10_2451) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public static method M(): int "
        "    using var a := new Array<int>(); "
        "    using var seq: IList<int> := a; "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasInertRhsWarning(r, "seq"));
}

}  // namespace

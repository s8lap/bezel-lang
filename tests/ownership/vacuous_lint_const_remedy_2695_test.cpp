// Ownership / diagnostics — the #2073 vacuous-`using var` lint told the author
// to write a binding the compiler REJECTS. Issue #2695.
//
// Spec, v0.240:
//   §28.11 "The element slot policy" — the operation table's `Dispose()` row
//     for a borrowing collection reads "no user-visible work (the backing
//     storage is part of the instance's allocation, released with it — §28.9)".
//     That is a statement about `Dispose()`, and it is TRUE.
//   §21.1 — a `const` local is never-owned; its initializer must not be an
//     owned production. Enforced at Ownership.cpp's `LocalConstDecl` arm:
//     "a `const` local (`x`) must not take an owned production — the binding
//     never takes a disposal obligation; bind owned values with `using var`
//     (§21.1 / §28.3)".
//   §28.3 auto-drop — a plain `var` owned local draws "declare it `using var`
//     to make the scope-bound disposal explicit".
//
// THE BUG. Both of the lint's messages end in "use `const x := …;`". For a
// FRESH PRODUCTION RHS that remedy does not compile, so the three spellings of
// the same program formed a closed loop with no warning-free member:
//
//   using var d := new Dictionary<string,string>();  → warn: "vacuous … use `const`"
//   const     d := new Dictionary<string,string>();  → ERROR §21.1: "… bind owned
//                                                       values with `using var`"
//   var       d := new Dictionary<string,string>();  → warn §28.3: "… declare it
//                                                       `using var`"
//
// Each diagnostic points at a spelling that another diagnostic rejects. The
// lint's premise ("no disposal obligation") is also independently wrong for a
// string-element instantiation — `Dictionary<K, string>`'s scope-end work is
// the per-element release #2682/#2686 added to `registerUsingDisposal` — but
// the FIX does not turn on that, because element-policy reasoning would leave
// `Dictionary<K, int>` (genuinely trivial, and equally unable to spell the
// remedy) still broken.
//
// THE FIX. Scope the lint to RHSs whose own suggested remedy is legal, by
// reusing §21.1's exact classifier — `producerState(init) != Owned` — on the
// lint's outer guard. The two sites now share one predicate and cannot
// disagree: the lint fires iff `const x := <same RHS>;` compiles.
//
// NOT A DISPOSAL CHANGE. This edit touches only the emission of a warning.
// `autoDropAtScopeEnd` / `registerUsingDisposal` are untouched, so the code
// generated for every program below is bit-for-bit what it was.

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// The inert-RHS ("Case A") warning, matched on its TEXT — the coarse
// has-a-diagnostic check cannot tell this warning from any other.
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

// The vacuous-borrow-pin ("Case B") warning — it carries the SAME `const`
// remedy, so the suppression must not merely push Case A's shapes into it.
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

// §21.1's owned-production rejection, matched on its TEXT. This is the
// predicate the whole fix is defined against: it decides whether the lint's
// advice is followable.
bool hasConstOwnedProductionError(const OwnershipResult& r,
                                  const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("must not take an owned production") ==
            std::string::npos)
            continue;
        if (d.message.find("`" + name + "`") == std::string::npos) continue;
        return true;
    }
    return false;
}

// Wrap a method body in the standard single-class compilation unit.
std::string prog(const std::string& body) {
    return "module M; class C "
           "  public static method M(): int " + body + " return 0; end M; "
           "  public static method Main(): int return 0; end Main; "
           "end C;";
}

// The "bound-then-pinned" alias. Through spec v0.345 these rows pinned the
// owned local itself (`var src := new <ty>(); using var p := src;`); spec
// v0.350 §28.10 *Adopting a named owned binding* makes that unmarked read of
// an owned local a compile error. The pinned source is now a BORROWING ALIAS
// of it (`var src := o;` borrows, §28.3), the borrow-state binding the rule
// leaves a borrow-pin.
std::string aliasProg(const std::string& ty) {
    return "module M; class C "
           "  public static method M(): int "
           "    var o := new " + ty + "(); var src := o; "
           "    using var p := src; return 0; end M; "
           "  public static method Main(): int return 0; end Main; "
           "end C;";
}

}  // namespace

// =====================================================================
// (1) The #2695 repro, both spellings. A fresh `Dictionary<string,string>`
// producer bound with `using var` must NOT draw the vacuous lint — `using var`
// is the only spelling §21.1 and §28.3 leave available for it.
// FAILS WITHOUT THE FIX (the warning fires).
// =====================================================================
BEZEL_TEST(vacuous_lint_const_remedy_2695,
           fresh_dictionary_producer_no_vacuous_warning_spec_28_10_21_1) {
    auto r = parseAndCheckOwnership(prog(
        "using var d: Dictionary<string, string> := "
        "new Dictionary<string, string>();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "d"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "d"));
}

// The un-annotated spelling of the SAME program. #2695 suspected the two
// disagreed (via the #2451 rework-4 `ld.type`-over-initializer precedence at
// Ownership.cpp:4578); measured on v0.240 they agreed all along — both fired.
// Pinned here so the pair can never drift apart, in either direction.
BEZEL_TEST(vacuous_lint_const_remedy_2695,
           annotated_and_unannotated_spellings_agree_spec_28_10) {
    auto annotated = parseAndCheckOwnership(prog(
        "using var d: Dictionary<string, string> := "
        "new Dictionary<string, string>();"));
    auto bare = parseAndCheckOwnership(prog(
        "using var d := new Dictionary<string, string>();"));
    BEZEL_EXPECT_EQ(hasInertRhsWarning(annotated, "d"),
                    hasInertRhsWarning(bare, "d"));
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(bare, "d"));
}

// =====================================================================
// (2) THE COHERENCE INVARIANT — the reason the fix is defined the way it is.
// Whatever the lint's message says, the compiler must accept. Asserting the
// §21.1 ERROR text directly is what makes (1) non-vacuous: without it, (1)
// only says "a warning stopped firing" and could be satisfied by deleting the
// lint. FAILS WITHOUT THE FIX in its first half.
// =====================================================================
BEZEL_TEST(vacuous_lint_const_remedy_2695,
           lint_fires_only_where_const_remedy_compiles_spec_21_1) {
    // A fresh production: remedy REJECTED (§21.1) ⟹ lint must stay silent.
    auto pinned = parseAndCheckOwnership(prog(
        "using var d := new Dictionary<string, string>();"));
    auto remedy = parseAndCheckOwnership(prog(
        "const d := new Dictionary<string, string>();"));
    BEZEL_EXPECT_TRUE(hasConstOwnedProductionError(remedy, "d"));
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(pinned, "d"));

    // A borrow of a bound local: remedy ACCEPTED ⟹ lint must still fire.
    auto pinned2 = parseAndCheckOwnership(prog(
        "var src := new Dictionary<string, string>(); using var d := src;"));
    auto remedy2 = parseAndCheckOwnership(prog(
        "var src := new Dictionary<string, string>(); const d := src;"));
    BEZEL_EXPECT_FALSE(remedy2.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasConstOwnedProductionError(remedy2, "d"));
    BEZEL_EXPECT_TRUE(hasInertRhsWarning(pinned2, "d"));
}

// =====================================================================
// (3) The whole borrowing family, both directions at once. Every one of the
// eight is a fresh production whose `const` remedy §21.1 rejects, so none may
// draw the lint; the same eight bound-then-pinned MUST still draw it (that is
// the rework-3c `rhsIsDisposableClassType` carve-out guard, preserved).
// FAILS WITHOUT THE FIX in its first half.
// =====================================================================
BEZEL_TEST(vacuous_lint_const_remedy_2695,
           borrowing_family_production_silent_alias_still_warns_spec_28_11) {
    const char* cases[] = {
        "Array<int>", "Stack<int>", "Queue<int>", "Set<int>",
        "SortedSet<int>", "Dictionary<string, int>",
        "SortedDictionary<string, int>", "LinkedList<int>",
        // The string-element instantiations #2695 was reported against: their
        // scope-end work is the per-element release of #2682/#2686, so the
        // lint's "no disposal obligation" wording was doubly wrong here.
        "Array<string>", "Dictionary<string, string>"};
    for (const char* ty : cases) {
        auto production = parseAndCheckOwnership(prog(
            std::string("using var p := new ") + ty + "();"));
        BEZEL_EXPECT_FALSE(production.diagnostics.hasErrors());
        BEZEL_EXPECT_FALSE(hasInertRhsWarning(production, "p"));
        BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(production, "p"));

        auto alias = parseAndCheckOwnership(aliasProg(ty));
        BEZEL_EXPECT_FALSE(alias.diagnostics.hasErrors());
        BEZEL_EXPECT_TRUE(hasInertRhsWarning(alias, "p"));
    }
}

// =====================================================================
// (4) OVER-SUPPRESSION GUARD — the shapes #2073 added the lint FOR are
// untouched. Each RHS below is a borrow or a value, so `producerState` is not
// Owned, the `const` remedy compiles, and the steering stays. A fix that
// disabled the lint wholesale passes (1)–(3) and fails here.
// =====================================================================
BEZEL_TEST(vacuous_lint_const_remedy_2695,
           borrow_and_value_rhs_still_warn_spec_28_10) {
    // Value-typed parameter — the original #2073 case.
    auto intParam = parseAndCheckOwnership(
        "module M; class C "
        "  public method M(b: int): int using var x := b; return 0; end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasInertRhsWarning(intParam, "x"));

    // Runtime-managed string literal.
    auto strLit = parseAndCheckOwnership(prog("using var s := \"hello\";"));
    BEZEL_EXPECT_TRUE(hasInertRhsWarning(strLit, "s"));

    // A call returning a value type — not an owned production.
    auto valueCall = parseAndCheckOwnership(
        "module M; class C "
        "  public static method F(): int return 42; end F; "
        "  public static method M(): int using var x := C.F(); return 0; end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasInertRhsWarning(valueCall, "x"));

    // A non-iteration-protocol sentinel over a bound array (#2451's boundary).
    auto ilist = parseAndCheckOwnership(prog(
        "var a := new Array<int>(); using var seq: IList<int> := a;"));
    BEZEL_EXPECT_TRUE(hasInertRhsWarning(ilist, "seq"));

    // Case B is likewise untouched: a borrow parameter's pin extends nothing.
    auto borrowParam = parseAndCheckOwnership(
        "module M; "
        "class Cell public constructor() end constructor; "
        "destructor end destructor; end Cell; "
        "class C "
        "  public method M(c: Cell): int using var x := c; return 0; end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasVacuousBorrowPinWarning(borrowParam, "x"));
}

// =====================================================================
// (5) The disposable-class negative controls stay negative — the new guard
// suppresses, it never un-suppresses. A fresh `Channel`/`Cell` production was
// already silent (it is a real disposal obligation); it must remain silent for
// the ORIGINAL reason as well, so `rhsIsDisposableClassType` is still reached
// for every non-production RHS of those types.
// =====================================================================
BEZEL_TEST(vacuous_lint_const_remedy_2695,
           disposable_class_controls_unchanged_spec_28_10) {
    auto fresh = parseAndCheckOwnership(
        "module M; "
        "class Cell public constructor() end constructor; "
        "destructor end destructor; end Cell; "
        "class C "
        "  public static method M(): int using var c := new Cell(); "
        "    return 0; end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(fresh, "c"));

    // Bound-then-pinned: reaches Case A's type test, which must still exclude
    // a disposable class (rework 2). This is the arm the production guard
    // could otherwise have masked.
    auto alias = parseAndCheckOwnership(
        "module M; "
        "class Cell public constructor() end constructor; "
        "destructor end destructor; end Cell; "
        "class C "
        "  public static method M(): int var src := new Cell(); "
        "    using var c := src; return 0; end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(alias, "c"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(alias, "c"));
}

// Ownership / diagnostics — the §28.10 vacuous-`using var` lint fired on
// FACTORY-CALL forms of the 13 built-in class heads the codegen auto-drop
// chain reclaims but the lint's `rhsIsDisposableClassType` table does NOT.
// Issue #3017.
//
// Spec, v0.240:
//   §28.9 "Runtime-managed types" — every type carries a disposal obligation
//     in one of two forms: the vtable slot-2 `Dispose()` or the §28.9
//     trivial-dispose reclaim (the compiler frees the undischarged instance at
//     its owner's release point).
//   §28.10 "Borrow-pinning consistency" — a `using var` over a disposable type
//     is the CORRECT canonical pattern, not vacuous; the scope-end disposal
//     does real work.
//   §28.3 "Auto-drop" — `autoDropAtScopeEnd` / `registerUsingDisposal` schedule
//     the per-scope reclaim; codegen carries the per-builtin release arms.
//
// THE BUG. The §28.10 vacuity lint's table
// (`common::kDisposableBuiltinHeadNames`, lifted from `rhsIsDisposableClassType`
// Path 2 by #2992) and the codegen auto-drop chain's per-arm claimed set
// (`Emitter::registerUsingDisposal`, LlvmBackend_Setup.cpp) DISAGREE on 13
// names: the 8 borrowing collections (Array/Stack/Queue/Set/SortedSet/
// Dictionary/SortedDictionary/LinkedList) and the 5 vtable-less builtins
// (Random/Atomic/AtomicInteger/Stopwatch/Buffer). Every one is chain-claimed
// (has a dedicated disposal arm) but absent from the lint's table. The
// disagreement manifested as false-positive vacuity warnings on FACTORY-CALL
// forms `using var x := C.Make()` of those types — measured on d2a3d3e for
// all 11 names tested (the borrowing collections + Stopwatch + Random + Buffer;
// Atomic/AtomicInteger likewise per the issue).
//
// Why they disagree (the structural cause). `rhsIsDisposableClassType` answers
// the LINT question ("should the §28.10 vacuity lint fire on this type?").
// The auto-drop chain answers the HEAP-OWNERSHIP question ("does this built-in
// own heap the auto-drop must release?"). These are different questions:
//   - The lint fires iff the `const` remedy is legal (#2695 coherence
//     invariant). A borrowing collection alias
//     (`var src := new Array<int>(); using var p := src;`) has a legal `const`
//     remedy, so the lint SHOULD fire on it — and it does, because Array is
//     NOT in the lint's table.
//   - But Array DOES own its backing (`bz_array_t*`), so the chain claims it.
//     A factory call `using var x := C.MakeArr()` SHOULD NOT fire the lint
//     (Array IS disposable), but it does because the lint's table doesn't know
//     about Array.
//
// THE FIX (option (b) from the issue). Extend `constInitializerRejected` with
// a new disjunct: a STATIC CallExpr whose return type is a chain-claimed
// built-in head (the 13 names in `common::kChainClaimedBuiltinHeadName`) is
// treated as "owned-production" for §21.1 purposes, so the §28.10 lint's
// outer guard `!constInitializerRejected` short-circuits and the warning does
// not fire. The alias case is preserved: the IdentifierExpr RHS bypasses every
// CallExpr disjunct AND `rhsIsDisposableClassType` Path 2 still returns false
// for the 13 heads, so the lint still fires on a bound-then-pinned alias of a
// borrowing collection.
//
// STATIC gating mirrors §28.3's structural-source rule: an instance method has
// a borrow source (the receiver) and may legitimately return a borrow of
// receiver state; a static method has no receiver, so a return is either a
// fresh `new T()` or a fresh built-in materializer, and the chain will
// reclaim it. A `using var x := obj.GetArr()` over an instance getter
// returning a borrowed field stays on today's "vacuous → use const" path —
// which is the correct spelling for a borrow.
//
// NOT A DISPOSAL CHANGE. This edit touches only the emission of a warning.
// `autoDropAtScopeEnd` / `registerUsingDisposal` are untouched, so the code
// generated for every program below is bit-for-bit what it was. The chain
// already reclaims each of the 13 names (verified by #2992's end-of-chain
// guard and the per-arm disposal logic).

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// The inert-RHS ("Case A") vacuity warning, matched on its TEXT — the coarse
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

// Wrap a method body in the standard single-class compilation unit.
std::string prog(const std::string& body) {
    return "module M; class C "
           "  public static method M(): int " + body + " return 0; end M; "
           "  public static method Main(): int return 0; end Main; "
           "end C;";
}

// The full set of 13 chain-claimed built-in heads absent from the lint's
// table — the population the issue identifies as a false-positive vacuity
// warning source on factory-call forms. Each is a `static` factory that
// returns a fresh instance of the corresponding built-in head.
std::string factoryProg(const std::string& head, const std::string& ctor) {
    return "module M; class C "
           "  public static method Make(): " + head + " "
           "    return " + ctor + "; "
           "  end Make; "
           "  public static method Main(): int "
           "    using var x := C.Make(); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

// For factory bodies that aren't a single `new T(...)` expression —
// `Stopwatch.StartNew()`, a built-in static factory — supply the full body
// of the `Make` method as a single string and let this helper wire up the
// `using var` use site.
// §2.3 / #5034: Stopwatch lives in System.Diagnostics; the import is required
// so the ExprTypecheck gate (§2.3) does not reject the generated program.
std::string factoryProgBody(const std::string& head,
                            const std::string& body) {
    return "module M; import System.Diagnostics; class C "
           "  public static method Make(): " + head + " " +
           body +
           "  end Make; "
           "  public static method Main(): int "
           "    using var x := C.Make(); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// =====================================================================
// (1) THE FACTORY-CALL FORM IS NOT VACUOUS for every chain-claimed head.
// Each test below asserts `using var x := C.Make()` over one of the 13
// names draws NEITHER Case A nor Case B. FAILS WITHOUT THE FIX — every
// one fired the vacuity warning on d2a3d3e.
// =====================================================================

BEZEL_TEST(vacuous_lint_factory_call_chain_claimed_3017,
           array_int_factory_call_no_vacuous_spec_28_9_28_10) {
    auto r = parseAndCheckOwnership(factoryProg("Array<int>",
                                                "new Array<int>(3)"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "x"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "x"));
}

BEZEL_TEST(vacuous_lint_factory_call_chain_claimed_3017,
           stack_int_factory_call_no_vacuous_spec_28_9_28_10) {
    auto r = parseAndCheckOwnership(factoryProg("Stack<int>",
                                                "new Stack<int>()"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "x"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "x"));
}

BEZEL_TEST(vacuous_lint_factory_call_chain_claimed_3017,
           queue_int_factory_call_no_vacuous_spec_28_9_28_10) {
    auto r = parseAndCheckOwnership(factoryProg("Queue<int>",
                                                "new Queue<int>()"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "x"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "x"));
}

BEZEL_TEST(vacuous_lint_factory_call_chain_claimed_3017,
           set_int_factory_call_no_vacuous_spec_28_9_28_10) {
    auto r = parseAndCheckOwnership(factoryProg("Set<int>",
                                                "new Set<int>()"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "x"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "x"));
}

BEZEL_TEST(vacuous_lint_factory_call_chain_claimed_3017,
           sorted_set_int_factory_call_no_vacuous_spec_28_9_28_10) {
    auto r = parseAndCheckOwnership(factoryProg("SortedSet<int>",
                                                "new SortedSet<int>()"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "x"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "x"));
}

BEZEL_TEST(vacuous_lint_factory_call_chain_claimed_3017,
           dictionary_string_int_factory_call_no_vacuous_spec_28_9_28_10) {
    auto r = parseAndCheckOwnership(factoryProg("Dictionary<string, int>",
                                                "new Dictionary<string, int>()"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "x"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "x"));
}

BEZEL_TEST(vacuous_lint_factory_call_chain_claimed_3017,
           sorted_dictionary_string_int_factory_call_no_vacuous_spec_28_9_28_10) {
    auto r = parseAndCheckOwnership(factoryProg(
        "SortedDictionary<string, int>", "new SortedDictionary<string, int>()"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "x"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "x"));
}

BEZEL_TEST(vacuous_lint_factory_call_chain_claimed_3017,
           linked_list_int_factory_call_no_vacuous_spec_28_9_28_10) {
    auto r = parseAndCheckOwnership(factoryProg("LinkedList<int>",
                                                "new LinkedList<int>()"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "x"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "x"));
}

BEZEL_TEST(vacuous_lint_factory_call_chain_claimed_3017,
           stopwatch_factory_call_no_vacuous_spec_28_9_28_10) {
    auto r = parseAndCheckOwnership(factoryProgBody(
        "Stopwatch",
        "    return Stopwatch.StartNew(); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "x"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "x"));
}

BEZEL_TEST(vacuous_lint_factory_call_chain_claimed_3017,
           random_factory_call_no_vacuous_spec_28_9_28_10) {
    auto r = parseAndCheckOwnership(factoryProgBody(
        "Random",
        "    return new Random(); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "x"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "x"));
}

BEZEL_TEST(vacuous_lint_factory_call_chain_claimed_3017,
           buffer_int_factory_call_no_vacuous_spec_28_9_28_10) {
    auto r = parseAndCheckOwnership(factoryProg("Buffer<int>",
                                                "new Buffer<int>(4)"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "x"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "x"));
}

BEZEL_TEST(vacuous_lint_factory_call_chain_claimed_3017,
           atomic_int_factory_call_no_vacuous_spec_28_9_28_10) {
    auto r = parseAndCheckOwnership(factoryProgBody(
        "Atomic<int>",
        "    return new Atomic<int>(0); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "x"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "x"));
}

BEZEL_TEST(vacuous_lint_factory_call_chain_claimed_3017,
           atomic_integer_int_factory_call_no_vacuous_spec_28_9_28_10) {
    auto r = parseAndCheckOwnership(factoryProgBody(
        "AtomicInteger<int>",
        "    return new AtomicInteger<int>(0); "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "x"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "x"));
}

// =====================================================================
// (2) THE ALIAS CASE STILL FIRES (#2695 COHERENCE INVARIANT).
// The IdentifierExpr RHS bypasses every CallExpr disjunct; rhsIsDisposable-
// ClassType Path 2 still returns false for the 13 heads (they are
// deliberately absent from `kDisposableBuiltinHeadNames` by #2695's
// invariant). So the lint must STILL fire on a bound-then-pinned alias of
// every chain-claimed head. FAILS WITHOUT THE FIX in the SAME way it
// fails today — these cases were the design point for the exclusion.
// =====================================================================

BEZEL_TEST(vacuous_lint_factory_call_chain_claimed_3017,
           alias_case_still_fires_for_chain_claimed_spec_28_11_28_10) {
    const char* cases[] = {
        "Array<int>", "Stack<int>", "Queue<int>", "Set<int>",
        "SortedSet<int>",
        "Dictionary<string, int>", "SortedDictionary<string, int>",
        "LinkedList<int>",
        "Buffer<int>",
    };
    for (const char* ty : cases) {
        auto alias = parseAndCheckOwnership(prog(
            std::string("var src := new ") + ty + "(); "
            "using var p := src;"));
        BEZEL_EXPECT_FALSE(alias.diagnostics.hasErrors());
        BEZEL_EXPECT_TRUE(hasInertRhsWarning(alias, "p"));
    }
}

// =====================================================================
// (3) STATIC GATING — an INSTANCE method returning one of the 13 heads
// stays on today's behaviour. The receiver may be a borrow source for the
// returned field, so the lint MUST keep firing: `const` is the correct
// spelling for a borrow of receiver state, and the chain's per-arm
// disposal at scope end would dispose a value it does not own.
//
// FAILS without the fix only if the fix accidentally widens to instance
// methods (a regression vs. #2695's coherence invariant on the alias
// arm). FAILS without the fix at all if today's behaviour already
// suppresses the warning for instance getters — which it does not, because
// the issue measures the warning on factory-call forms only.
// =====================================================================

BEZEL_TEST(vacuous_lint_factory_call_chain_claimed_3017,
           instance_method_returning_chain_claimed_unchanged_spec_28_3) {
    // An instance getter returning a borrow of an owned field. Today's
    // behaviour: the lint fires (the call's `producerState` is Borrowed and
    // `rhsIsDisposableClassType` returns false for the head). My fix is
    // gated to STATIC callees so the warning still fires here — `const`
    // is the correct spelling for a borrow.
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  private _arr: Array<int>; "
        "  public constructor() _arr := new Array<int>(3); end constructor; "
        "  public method Get(): Array<int> return _arr; end Get; "
        "  public static method M(): int "
        "    var c := new C(); "
        "    using var x := c.Get(); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasInertRhsWarning(r, "x"));
}

// =====================================================================
// (4) OVER-REJECTION GUARD — the existing #2695 lint behaviour is not
// weakened for non-chain-claimed types. A fresh `Dictionary<string,int>`
// production still does NOT fire (the production form is owned-production
// via the `producerState==Owned` gate, the path that already worked). A
// borrow of a bound local still DOES fire. A value-typed parameter still
// fires. Each case probes a distinct disjunct of `constInitializerRejected`
// to ensure none of them regressed.
// =====================================================================

BEZEL_TEST(vacuous_lint_factory_call_chain_claimed_3017,
           over_rejection_guard_disposable_class_production_silent_spec_28_10) {
    // A USER class with a destructor (a non-trivial-dispose class) — the
    // path #2073 added for. The production form must NOT fire.
    auto r = parseAndCheckOwnership(
        "module M; "
        "class Cell public constructor() end constructor; "
        "destructor end destructor; end Cell; "
        "class C "
        "  public static method M(): int using var c := new Cell(); "
        "    return 0; end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "c"));
}

BEZEL_TEST(vacuous_lint_factory_call_chain_claimed_3017,
           over_rejection_guard_value_param_still_warns_spec_28_10) {
    // A value-typed parameter — the original #2073 case.
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public method M(b: int): int using var x := b; return 0; end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasInertRhsWarning(r, "x"));
}

BEZEL_TEST(vacuous_lint_factory_call_chain_claimed_3017,
           over_rejection_guard_string_literal_still_warns_spec_28_10) {
    // Runtime-managed string literal.
    auto r = parseAndCheckOwnership(prog("using var s := \"hello\";"));
    BEZEL_EXPECT_TRUE(hasInertRhsWarning(r, "s"));
}

BEZEL_TEST(vacuous_lint_factory_call_chain_claimed_3017,
           over_rejection_guard_value_call_still_warns_spec_28_10) {
    // A call returning a value type — not an owned production.
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method F(): int return 42; end F; "
        "  public static method M(): int using var x := C.F(); return 0; end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasInertRhsWarning(r, "x"));
}

BEZEL_TEST(vacuous_lint_factory_call_chain_claimed_3017,
           over_rejection_guard_non_chain_call_returning_int_still_warns_spec_28_10) {
    // A call returning a built-in NON-chain-claimed type (e.g. an int or
    // a struct — but here, more interesting, a non-named return). My
    // disjunct gates on `rt->kind == Named`, so a value-typed return
    // never matches.
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method F(): int return 42; end F; "
        "  public static method M(): int "
        "    using var x := C.F(); "
        "    return 0; "
        "  end M; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasInertRhsWarning(r, "x"));
}

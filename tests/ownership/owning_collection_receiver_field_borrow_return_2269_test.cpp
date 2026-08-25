// Spec-derived regression tests for #2269 — §28.6 L6999–L7003 worked example,
// restored from #1760 over the spec admission.
//
// §28.6 (v0.228/v0.240) admits a bare read of an `owned` owning-collection
// field of THE RECEIVER (`this`) returned through a non-`owned` owning-
// collection return type, with the worked example:
//
//   public method GetTransactions(): OwningArray<Transaction>
//     return _transactions;     // OK: borrows the field — the caller may
//                               //   read/iterate/mutate/drain it but not dispose it.
//   end GetTransactions;
//
// The compiler had been rejecting this pattern since #1760 (the wider
// owning-collection analogue of #1660's deferred-activation fix). Per
// CLAUDE.md rule 1 the spec is the source of truth; #2269 restores the
// receiver-owned-field carve-out at the definition site (the same
// this-restricted predicate `exprIsReceiverOwnedFieldBorrow(e)` the WRAPPER
// branch uses for Shared/Weak/ChannelSender/CancellationTokenSource). The
// indirect-dispatch unsoundness the carve-out reintroduces is filed
// separately as a spec-amendment follow-up (not a definition-site rejection).
//
// This file pins the SPEC worked example (the §28.6 L6999–L7003 code) plus
// every owning-COLLECTION head and every admission surface the spec
// enumerates, so a future regression in `checkBorrowReturnedThroughOwningType`
// cannot silently re-flip the carve-out without these tests turning red.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// `module M; class Cell …; ` prelude — every test below needs a Cell class
// the OwningArray<Cell> field can hold. The destructor is empty because the
// double-free the indirect-dispatch hazard WOULD cause needs no user-visible
// code (#1760's measurement); the spec admission test is at the definition
// site only, where user code does not run.
std::string mod(const std::string& body) {
    return std::string(
        "module M; "
        "class Cell "
        "  public constructor() end constructor; "
        "  destructor end destructor; "
        "end Cell; ") +
        body;
}

// Mirrors `vacuous_pin_lint_2073_test.cpp`'s file-local helpers (not yet
// lifted to `ownership_support.hpp`). The §28.10 vacuous-pin lint fires
// on `using var x := <non-disposable>` and warns; we need the negative
// case for the StringBuilder trivial-dispose admission (the lint MUST
// stay silent on `using var sb := new StringBuilder()`).
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

bool hasVacuousBorrowPinWarning(const OwnershipResult& r,
                                const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("is vacuous") == std::string::npos) continue;
        if (d.message.find("RHS is a borrow of `") == std::string::npos)
            continue;
        if (d.message.find("`" + name + "`") == std::string::npos) continue;
        return true;
    }
    return false;
}

}  // namespace

// ===========================================================================
// THE SPEC WORKED EXAMPLE — §28.6 L6999–L7003, verbatim. Must compile.
// ===========================================================================

// `public method GetTransactions(): OwningArray<Transaction>` returning
// `return _transactions;` — the §28.6 admission. Verbatim against the spec
// (Transaction is not in the test's prelude; rename to the prelude's Cell so
// the test compiles standalone; the shape is identical).
BEZEL_TEST(section_28_6_2269, worked_example_get_transactions_borrows_field) {
    auto r = parseAndCheckOwnership(mod(
        "class Account "
        "  public owned _transactions: OwningArray<Cell>; "
        "  public constructor() _transactions := new OwningArray<Cell>(); end constructor; "
        "  destructor end destructor; "
        "  public method GetTransactions(): OwningArray<Cell> "
        "    return _transactions; "
        "  end GetTransactions; "
        "end Account;"));
    // §28.6 admission — must compile, no diagnostics. FAILS without the #2269
    // carve-out restoration (pre-#2269 rejected `return _transactions;`
    // because #1760 removed the receiver-owned-field carve-out for the
    // owning collections).
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// All six `kOwningCollectionHeads` admit the same shape.
// ===========================================================================

BEZEL_TEST(section_28_6_2269,
           owning_dictionary_receiver_field_borrow_returned) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned field: OwningDictionary<int, Cell>; "
        "  public constructor() field := new OwningDictionary<int, Cell>(); end constructor; "
        "  destructor end destructor; "
        "  public method Get(): OwningDictionary<int, Cell> return field; end Get; "
        "end Holder;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(section_28_6_2269, owning_set_receiver_field_borrow_returned) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned field: OwningSet<Cell>; "
        "  public constructor() field := new OwningSet<Cell>(); end constructor; "
        "  destructor end destructor; "
        "  public method Get(): OwningSet<Cell> return field; end Get; "
        "end Holder;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(section_28_6_2269, owning_stack_receiver_field_borrow_returned) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned field: OwningStack<Cell>; "
        "  public constructor() field := new OwningStack<Cell>(); end constructor; "
        "  destructor end destructor; "
        "  public method Get(): OwningStack<Cell> return field; end Get; "
        "end Holder;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(section_28_6_2269, owning_queue_receiver_field_borrow_returned) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned field: OwningQueue<Cell>; "
        "  public constructor() field := new OwningQueue<Cell>(); end constructor; "
        "  destructor end destructor; "
        "  public method Get(): OwningQueue<Cell> return field; end Get; "
        "end Holder;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(section_28_6_2269,
           owning_linked_list_receiver_field_borrow_returned) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned field: OwningLinkedList<Cell>; "
        "  public constructor() field := new OwningLinkedList<Cell>(); end constructor; "
        "  destructor end destructor; "
        "  public method Get(): OwningLinkedList<Cell> return field; end Get; "
        "end Holder;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// The carve-out is THIS-RESTRICTED. A borrowed PARAMETER / LOCAL / a field
// of ANOTHER object is NOT covered by §28.6 — those still fall through to
// the borrow rejection (the source need not outlive the call).
// ===========================================================================

// A borrowed PARAMETER (the `=> a` shape — the source `a` is a borrow, the
// caller would dangle past `a`'s scope end).
BEZEL_TEST(section_28_6_2269,
           owning_array_borrowed_param_return_still_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Identity(a: OwningArray<Cell>): OwningArray<Cell> "
        "    => a; "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrowed `OwningArray<…>` value may not be returned"));
}

// A field of ANOTHER object — the source's lifetime is bound to `other`'s,
// which the caller does not hold across the call.
BEZEL_TEST(section_28_6_2269,
           owning_array_other_object_field_return_still_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class Holder "
        "  public owned _items: OwningArray<Cell>; "
        "  public constructor(owned f: OwningArray<Cell>) _items := f; end constructor; "
        "  destructor end destructor; "
        "end Holder; "
        "class P "
        "  public static method Take(other: Holder): OwningArray<Cell> "
        "    return other._items; "
        "  end Take; "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "a borrowed `OwningArray<…>` value may not be returned"));
}

// A fresh value through a non-`owned` owning return — the #1008 Part C
// tightening still rejects this (the caller cannot distinguish a fresh
// owned value from a borrow by the return signature; an `owned` return
// type is required for fresh).
BEZEL_TEST(section_28_6_2269,
           owning_array_fresh_value_nonowned_return_still_rejected) {
    auto r = parseAndCheckOwnership(mod(
        "class P "
        "  public static method Make(): OwningArray<Cell> "
        "    return new OwningArray<Cell>(); "
        "  end Make; "
        "end P;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ===========================================================================
// StringBuilder (§28.9 trivial-dispose) — the spec's own §17.13 / §22.5 /
// §28.9 L7363 cases. None of these draw the §28.3 ceremony warning the
// #2190 commit erroneously added (it cited a stale v0.226 reading). The
// `using var` shape and the plain-`var` shape BOTH pass without the
// ownership-discharge warning the spec §28.9 L7357 forbids.
// ===========================================================================

// §17.13 / §28.9 — `_ := new StringBuilder();` — the discard form. The
// value is freed at end of statement, no user code running; the §28.9
// ceremony exemption applies (§28.9 L7357).
BEZEL_TEST(section_28_6_2269, stringbuilder_discard_no_ceremony_warning) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public method Use(): int "
        "    _ := new StringBuilder(); "
        "    return 0; "
        "  end Use; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §21 / §28.9 — `var sb := new StringBuilder();` — the plain `var` form.
// §28.9 L7357: a `new` of a trivial-dispose class type whose result is not
// stored in `owned` or `using var` does NOT produce the standard ownership-
// discharge warning. The scope-end reclaim still runs (codegen chain routes
// through the borrowing-`Array<string>` free path, #1185 /
// LlvmBackend_Setup.cpp:15725) but it is the trivial-dispose reclaim, not
// the must-use discharge.
BEZEL_TEST(section_28_6_2269,
           stringbuilder_plain_var_no_ceremony_warning_spec_28_9_l7357) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public method Plain(): int "
        "    var sb := new StringBuilder(); "
        "    return sb.Length; "
        "  end Plain; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // §28.9 L7357: trivial-dispose `new` expression whose result is not
    // stored in `owned` or `using var` does NOT produce the standard
    // ownership-discharge warning. The scope-end reclaim still runs
    // (codegen chain), but it is the trivial-dispose reclaim, not a
    // must-use discharge. FAILS without the #2269 StringBuilder removal
    // from `kDisposableBuiltinHeadNames` and addition to
    // `kChainClaimedBuiltinHeadNames` — pre-#2269 the §28.3 ceremony
    // over-fired on the trivial-dispose case.
    BEZEL_EXPECT_FALSE(bezel::test::hasAutoDropWarning(r, "sb"));
}

// `using var` StringBuilder (§28.9 L7361) — permits the form; no vacuous-
// pin warning; the scope-end free runs the borrowing-`Array<string>` free
// path with the string-element release thunk. The §28.9 ceremony exemption
// extends: a `using var` on a trivial-dispose class is permitted and has
// no observable effect other than scope limitation.
BEZEL_TEST(section_28_6_2269,
           stringbuilder_using_var_no_vacuous_pin_spec_28_9_l7361) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public method Using(): int "
        "    using var sb := new StringBuilder(); "
        "    return sb.Length; "
        "  end Using; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasInertRhsWarning(r, "sb"));
    BEZEL_EXPECT_FALSE(hasVacuousBorrowPinWarning(r, "sb"));
}

// An explicit-`Dispose()` on a StringBuilder — admitted (§28.9 trivial-
// dispose Dispose is a no-op, but the explicit form is legal and the
// call must reach codegen). Pre-#2269 the call was already wired (the
// codegen chain reclaims via the borrowing-`Array<string>` path on `using
// var` scope-end, and the explicit-Dispose lowering was added by #2921).
// This test pins that the parse-time/ownership-time classification still
// permits the call after #2269.
BEZEL_TEST(section_28_6_2269, stringbuilder_explicit_dispose_compiles) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "class C "
        "  public method Use(): int "
        "    var sb := new StringBuilder(); "
        "    sb.Dispose(); "
        "    return 0; "
        "  end Use; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

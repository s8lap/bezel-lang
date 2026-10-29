// Spec-derived regression tests for issue #7975 — a bare read of a field
// typed at a sync-enumerator head, stored into an `owned` enumerator field of
// another (or the same) object, was admitted by the §28.3 borrowed-field
// store check while the class-typed twin is rejected.
//
// §28.3 (v0.350): "A borrow may be stored in a field of an object only if the
// borrow's source provably outlives that object."
// §28.5 (v0.350): an `owned` field is released by its holder's disposal
// cascade — the frame `h.En` holds is owned by `h`.
//
// THE BUG (measured on the pre-fix compiler, r37-rev-l5f r08 / r08c):
//
//   using var h := new H(); h.Fill();
//   using var h2 := new H(); h2.En := h.En!;   → ACCEPTED; with the #7422
//                                                 cascade both holders release
//                                                 the one frame (ASan
//                                                 heap-use-after-free)
//   using var h := new H(); h.Fill();
//   h.B2 := h.B!;   (owned B, B2: Box?)         → REJECTED, "borrow of `h`
//                                                 stored into a field would
//                                                 outlive its source"
//
// ROOT CAUSE: `MethodOwnership::isBorrowableType` reads every
// declaration-less builtin head (`IEnumerator<T>`, `IOwningEnumerator<T>`
// among them) as non-borrowable, so `borrowSourceForExpr`'s member-read leg
// never names `h` as the source of `h.En!` and `checkBorrowStoreEscape`
// found nothing to reject.
//
// THE FIX: `checkBorrowStoreEscape` asks a dedicated resolver
// (`ownedEnumeratorFieldReadStoreSource`) for the field-destination case: a
// member read typed at a sync-enumerator head whose root is an owned
// scope-bound local (or an `owned` parameter) is a borrow of that root, and
// the existing §28.3 diagnostic fires. A fresh producer (`G.It()`) and a
// `move` out of the owned field keep their admission.
#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

const char* kHdr =
    "module M; "
    "class G "
    "  public static method Gen(): IEnumerable<int> "
    "    try "
    "      yield return 1; "
    "      yield return 2; "
    "    finally "
    "      Console.WriteLine(\"fin\"); "
    "    end; "
    "  end Gen; "
    "  public static method It(): IEnumerator<int> "
    "    try "
    "      yield return 5; "
    "    finally "
    "      Console.WriteLine(\"fin\"); "
    "    end; "
    "  end It; "
    "end G; "
    "class H "
    "  public owned En: IEnumerator<int>? := null; "
    "  public owned E2: IEnumerator<int>? := null; "
    "  public method Fill() "
    "    En := G.Gen().GetEnumerator(); "
    "  end Fill; "
    "end H; ";

std::string prog(const std::string& body) {
    return std::string(kHdr) +
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C; ";
}

const char* kNeedle = "borrow of `h` stored into a field would outlive its source";

}  // namespace

// r08: the borrow of `h`'s owned enumerator field stored into ANOTHER
// object's owned enumerator field. §28.3 (v0.350).
BEZEL_TEST(owned_enumerator_field_borrow_store_7975,
           other_object_owned_enumerator_field_rejected_spec_28_3) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    using var h := new H(); h.Fill(); "
        "    var ok := h.En!.MoveNext(); "
        "    using var h2 := new H(); "
        "    h2.En := h.En!; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kNeedle));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "§28.3"));
}

// r08c: the same borrow stored into a SIBLING owned field of the same
// object. §28.3 (v0.350).
BEZEL_TEST(owned_enumerator_field_borrow_store_7975,
           sibling_owned_enumerator_field_rejected_spec_28_3) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    using var h := new H(); h.Fill(); "
        "    var ok := h.En!.MoveNext(); "
        "    h.E2 := h.En!; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kNeedle));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "§28.3"));
}

// The nullable read without `!` is the same borrow. §28.3 (v0.350).
BEZEL_TEST(owned_enumerator_field_borrow_store_7975,
           nullable_read_without_bang_rejected_spec_28_3) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    using var h := new H(); h.Fill(); "
        "    h.E2 := h.En; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kNeedle));
}

// A non-`using` owned local root is the same scope-bound source. §28.3
// (v0.350).
BEZEL_TEST(owned_enumerator_field_borrow_store_7975,
           plain_owned_local_root_rejected_spec_28_3) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    var h := new H(); h.Fill(); "
        "    using var h2 := new H(); "
        "    h2.En := h.En!; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kNeedle));
}

// The `IOwningEnumerator<T>` head is the sync-enumerator family's other
// member and is classified identically. §28.3 (v0.350).
BEZEL_TEST(owned_enumerator_field_borrow_store_7975,
           iowningenumerator_head_rejected_spec_28_3) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M; "
        "class R "
        "  public constructor() end constructor; "
        "end R; "
        "class H "
        "  public owned En: IOwningEnumerator<R>? := null; "
        "  public owned E2: IOwningEnumerator<R>? := null; "
        "  public method Gen(): IOwningEnumerable<R> "
        "    yield return new R(); "
        "  end Gen; "
        "  public method Fill() "
        "    En := Gen().GetOwningEnumerator(); "
        "  end Fill; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    using var h := new H(); h.Fill(); "
        "    h.E2 := h.En!; "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kNeedle));
}

// CONTROL — a fresh producer stored into the owned field transfers a frame
// the field owns outright; no borrow, no error. §28.3 Producers (v0.350).
BEZEL_TEST(owned_enumerator_field_borrow_store_7975,
           fresh_producer_store_control_accepted_spec_28_3) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    using var h := new H(); h.Fill(); "
        "    h.E2 := G.It(); "));
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, "would outlive its source"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// CONTROL — `move` out of the owned field through the owned local transfers
// the frame (§28.8); the store is not a borrow.
BEZEL_TEST(owned_enumerator_field_borrow_store_7975,
           move_out_of_owned_field_control_accepted_spec_28_8) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "    using var h := new H(); h.Fill(); "
        "    h.E2 := move h.En; "));
    BEZEL_EXPECT_FALSE(hasErrorMessage(r, "would outlive its source"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

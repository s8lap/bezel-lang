// fix #1157 / stdlib §4.14: for a CLASS-typed T, `OwnedBuffer<T>` stores `T?`
// in each slot (nullable references, all null until set), and its indexer is
// declared `[i: int]: T?`. The sema indexer-get must therefore be typed `T?`
// (nullable) — NOT the bare non-nullable `T` it was folded to via the
// `isArrLike` path alongside `OwnedArray<T>`.
//
// §4.14 (class-typed T): "fixed-at-construction storage of N slots, each
// holding `T?` (a nullable reference) … all slots initialized to null. The
// indexer's setter takes ownership of the assigned value … The natural
// restoration for a nullable slot is `buf[i] := null`."
//
// Two bugs flowed from the mistyping:
//   (1) ACCEPT-TRAP: `var x := buf[0]; return x.v;` over an empty (null) slot
//       compiled clean and null-dereferenced at runtime (SIGSEGV). With the
//       slot typed `T?`, the member access requires a null guard / `?.`.
//   (2) `buf[i] := null` was rejected ("assigned value is nullable but target
//       is non-nullable §7") because the setter target was typed non-nullable.
//
// A VALUE-typed T (primitive / record / enum) stores `T` directly with value
// defaults, so its indexer stays non-nullable `T` — the change is gated on a
// class/interface element type only (no regression for value-typed buffers).

#include <string_view>

#include "support/codegen_support.hpp"
#include "support/ownership_support.hpp"
#include "support/sema_support.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;
using bezel::test::parseAndCheckOwnership;

namespace {

int runFull(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasNullableNarrowingError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("on a nullable value requires narrowing") !=
            std::string::npos)
            return true;
    return false;
}

const char* kCell =
    "module M; "
    "class Cell public v: int; "
    "  public constructor(x: int) this.v := x; end constructor; end Cell; ";

}  // namespace

// §4.14 (the #1157 repro): an empty-slot read of a class-typed OwnedBuffer is
// `Cell?` — a member access `.v` without a null guard is a clean sema error
// (previously it compiled clean and SIGSEGVed).
BEZEL_TEST(ownedbuffer_class_slot_nullable_1157, class_empty_slot_read_rejected) {
    auto r = parseSemaAll(
        std::string(kCell) +
        "class C public static method Main(): int "
        "  using var buf := new OwnedBuffer<Cell>(2); "
        "  var x := buf[0]; "
        "  return x.v; "  // null deref on the empty (null) slot — now rejected
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNullableNarrowingError(r));
}

// §4.14: `buf[i] := null` is the natural §28.8.1 restoration for a class-typed
// slot — it must be ACCEPTED (the setter target is `T?`). Previously rejected.
BEZEL_TEST(ownedbuffer_class_slot_nullable_1157, class_assign_null_accepted) {
    auto r = runJit(
        std::string(kCell) +
        "class C public static method Main(): int "
        "  using var buf := new OwnedBuffer<Cell>(2); "
        "  buf[0] := new Cell(5); "
        "  buf[0] := null; "  // restore the slot to null — accepted
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// §4.14: a GUARDED read of a class-typed slot works — narrow with `<> null`,
// then force-unwrap `buf[i]!` to reach the member.
BEZEL_TEST(ownedbuffer_class_slot_nullable_1157, class_guarded_read_works) {
    BEZEL_EXPECT_EQ(runFull(
        std::string(kCell) +
        "class C public static method Main(): int "
        "  using var buf := new OwnedBuffer<Cell>(2); "
        "  buf[0] := new Cell(7); "
        "  if buf[0] <> null then return buf[0]!.v; end; "
        "  return -1; "
        "end Main; end C;"), 7);
}

// §4.14 (value-typed T, regression guard): an `OwnedBuffer<int>` slot holds
// `int` directly (value default 0) — the indexer stays NON-nullable `int`, so a
// bare read needs no guard. `buf[1] := 42` then `buf[1]` reads 42.
BEZEL_TEST(ownedbuffer_class_slot_nullable_1157, value_int_indexer_non_nullable) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C public static method Main(): int "
        "  using var buf := new OwnedBuffer<int>(3); "
        "  buf[1] := 42; "
        "  var x := buf[1]; "  // non-nullable int — no guard required
        "  return x; "
        "end Main; end C;"), 42);
}

// §4.14 (value-typed T, regression guard): a RECORD element is value-typed too
// (slots hold the record directly) — the indexer stays non-nullable, so a bare
// `buf[i].field` read is admitted (records are §9 value types).
BEZEL_TEST(ownedbuffer_class_slot_nullable_1157, value_record_indexer_non_nullable) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; record Pt(x: int, y: int); "
        "class C public static method Main(): int "
        "  using var buf := new OwnedBuffer<Pt>(2); "
        "  buf[0] := new Pt(3, 4); "
        "  var p := buf[0]; "
        "  return p.x + p.y; "  // 7 — value-typed, no guard
        "end Main; end C;"), 7);
}

// §28.11 / §4.14 (#1209): OwnedBuffer<int> (value-typed element) indexer-set
// from a bare local variable must COMPILE (the slot receives a value COPY, not
// an owned transfer) and the local must remain usable after the assignment.
// Previously rejected as a BAD_REJECT because indexerSetterTakesOwned returned
// true unconditionally for all built-in owning collections, regardless of the
// element type's category.
BEZEL_TEST(ownedbuffer_class_slot_nullable_1157, owned_value_element_indexer_set_ok_1209) {
    BEZEL_EXPECT_EQ(runFull(
        "module M; class C public static method Main(): int "
        "  using var buf := new OwnedBuffer<int>(2); "
        "  var x := 4; "
        "  buf[0] := x; "         // value copy — x must not be consumed
        "  var y := x; "          // x still usable after the assignment
        "  return buf[0] + y; "   // 4 + 4 = 8
        "end Main; end C;"), 8);
}

// §28.11 / §4.14 (#1209 regression guard): OwnedBuffer<T> with a CLASS element
// type must still treat the indexer-set as an owned-sink — a bare owned local
// passed as the RHS is consumed (moved into the slot). A subsequent use of that
// local is rejected at compile time, preventing a double-dispose.
BEZEL_TEST(ownedbuffer_class_slot_nullable_1157,
           owned_class_element_indexer_set_still_moves_1209) {
    // Attempting to use `cell` after `buf[0] := cell` must produce an owned-
    // already-moved error (§28.3). If it compiles silently, the class element
    // branch of indexerSetterTakesOwned has regressed to non-consuming.
    // parseAndCheckOwnership runs the full pipeline including ownership checks.
    auto r = parseAndCheckOwnership(
        std::string(kCell) +
        "class C public static method Main(): int "
        "  using var buf := new OwnedBuffer<Cell>(2); "
        "  var cell := new Cell(42); "
        "  buf[0] := cell; "         // owned-sink: cell is consumed here
        "  var v := cell.v; "        // must be rejected — cell was moved out
        "  return v; "
        "end Main; end C;");
    // The program must not compile clean: the double-use of `cell` is an error.
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ===========================================================================
// §7.4 / #1208: Span<T> and ReadOnlySpan<T> over an OwnedBuffer<T> store T?
// in each slot for class/interface T (same semantics as OwnedBuffer<T> itself).
// ACCEPT-TRAP: bare member access `s[i].member` must be rejected (requires
// nullable narrowing).  BAD-REJECT: `s[i] := null` must be accepted.
// ===========================================================================

// §7.4 (#1208 repro): Span<Cell>[i] is typed Cell? — assigning `s[i]` to a
// local gives a `Cell?` local; a bare member access on it then requires
// narrowing (§7.4).  Previously the slot was typed non-nullable Cell so the
// member access compiled clean (accept-trap → null-deref at runtime).
BEZEL_TEST(ownedbuffer_class_slot_nullable_1157,
           span_class_bare_member_access_rejected_1208) {
    auto r = parseSemaAll(
        std::string(kCell) +
        "class C public static method Main(): int "
        "  using var buf := new OwnedBuffer<Cell>(2); "
        "  var s := buf.AsSpan(); "
        "  var x := s[0]; "  // x is Cell? (slot is null until set)
        "  return x.v; "     // member access on nullable — must be rejected
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNullableNarrowingError(r));
}

// §7.4 (#1208): Span<Cell>[i] := null — assigning null to a class-typed slot
// is the natural §28.8.1 restoration; it must be ACCEPTED.
// Previously rejected ("target is non-nullable").
BEZEL_TEST(ownedbuffer_class_slot_nullable_1157,
           span_class_assign_null_accepted_1208) {
    auto r = parseSemaAll(
        std::string(kCell) +
        "class C public static method Main(): int "
        "  using var buf := new OwnedBuffer<Cell>(2); "
        "  var s := buf.AsSpan(); "
        "  s[0] := null; "  // restore slot to null — must be accepted
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §7.4 (#1208): ReadOnlySpan<Cell>[i] is also typed Cell? — same fix applies.
// Assign the indexed slot to a local so the member access is on a nullable
// local variable (triggering the "nullable value" diagnostic path).
BEZEL_TEST(ownedbuffer_class_slot_nullable_1157,
           readonly_span_class_bare_member_access_rejected_1208) {
    auto r = parseSemaAll(
        std::string(kCell) +
        "class C public static method Main(): int "
        "  using var buf := new OwnedBuffer<Cell>(2); "
        "  var s := buf.AsReadOnlySpan(); "
        "  var x := s[0]; "  // x is Cell?
        "  return x.v; "     // must be rejected — nullable narrowing required
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNullableNarrowingError(r));
}

// §7.4 (#1208 regression guard): Span<int>[i] is `int` (value-typed T) — the
// indexer must remain NON-nullable, so a bare read needs no guard.
BEZEL_TEST(ownedbuffer_class_slot_nullable_1157,
           span_value_int_indexer_non_nullable_1208) {
    auto r = parseSemaAll(
        "module M; class C public static method Main(): int "
        "  using var buf := new OwnedBuffer<int>(2); "
        "  var s := buf.AsSpan(); "
        "  s[0] := 99; "
        "  var x := s[0]; "  // non-nullable int — no guard required
        "  return x; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

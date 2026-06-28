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
#include "support/sema_support.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

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

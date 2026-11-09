// fix #7852 — a borrow-bearing inline array `[2]Cls` was ACCEPTED at an
// `Object` parameter (`Console.WriteLine(m)`, `w.WriteLine(m)`).
//
// THE DEFECT (measured on a30bb11d, this lane's base):
//
//   module Conf;
//   import System.IO;
//   class Cls public constructor() end constructor; end Cls;
//   class P
//    public static method Main()
//    var w: TextWriter := Console.Out;
//    using var k1 := new Cls(); using var k2 := new Cls();
//    var m: [2]Cls := [k1, k2];
//    w.WriteLine(m);        // COMPILED
//    Console.WriteLine(m);  // COMPILED
//    end Main; end P;
//
// both calls compile and run — a silent over-acceptance. The RECORD TWIN
// (`record Pair(A: Cls, B: Cls)`, identical borrows) is rejected correctly, so
// the rule was reached and only the inline-array shape escaped it.
//
// THE MECHANISM. `ExprTC::checkBorrowErasingPosition`
// (`src/sema/ExprTypecheck.cpp`) is the ONE definition of the rule (it carries
// the `#5659` banner). It computed:
//
//   sourceIsBorrowBearing = sourceIsIReadOnlySpan ||
//                           common::isBorrowBearingRecordOrSpan(from, ctx_);
//   sourceIsNonOwningInlineArray = from->kind == InlineArray && !from->arrayOwned;
//   if (!sourceIsBorrowBearing || sourceIsNonOwningInlineArray ||
//       common::isIReadOnlySpanType(to))
//       return;                       // ← the escape
//
// `common::isBorrowBearingRecordOrSpan` (`src/common/OwnershipFacts.cpp`)
// ALREADY classifies `[2]Cls` as borrow-bearing — its `#5627` inline-array
// leg returns true when the element type is a reference type — so
// `sourceIsBorrowBearing` was TRUE for `m`, and the
// `sourceIsNonOwningInlineArray` disjunct then short-circuited the guard. The
// inline array was the ONLY value kind that could be simultaneously
// "borrow-bearing" and "not rejected": the record twin has no such escape.
//
// THE SPEC (the escape's own justifying comment was STALE — it cited §8.6 at
// v0.315, which the CURRENT revision supersedes):
//
//   * §15.1.2 (spec line 3004, *Lifetime of inline arrays carrying borrows*):
//     "An inline array without the `owned` marker is borrow-bearing (§3.6)
//      exactly when its element type, nullability stripped, is a class or
//      interface type that is not runtime-managed (§28.9), a borrow-bearing
//      record (§9.7), or a choice that holds borrows (§35.4). `[N]C` or
//      `[N]C?` over a class or interface `C` carries N (potentially null)
//      borrows in its slots".
//   * §8.6 (spec line 1256, *Boxing of inline arrays*): "Non-owning inline
//      arrays (`[N]T` and `[N]T?`) are ordinary value types and box per the
//      general rule. When the array is borrow-bearing (§15.1.2), the boxed
//      array carries borrows; the same borrow-lifetime constraints apply as for
//      boxed borrow-bearing records (above)."
//   * §8.6 (spec line 1251, *Boxing of borrow-bearing records*): "a value that
//      holds borrows (§3.6) is converted to `Object` or to an interface type
//      at exactly **one position**: as the **receiver** of a member call …
//      Every other conversion — at a parameter, plain or `scoped` … at a
//      local, a field, a collection slot … — is a **compile error at the
//      conversion**".
//   * §9.7 (spec line 1558, the *borrow-erasing position* bullet): "A value
//      that holds borrows may not reach a **borrow-erasing position**: a
//      position declared `Object` or an interface type — a parameter, plain
//      or `scoped`, a local, a field, a record component, a choice payload,
//      an inline-array slot, a collection slot or collection-literal element, a
//      `ref` or `out` location, a return slot — … The conversion … is a
//      compile error at the conversion site".
//   * §3.6 (spec lines 541 / 543, the derived-type-properties table):
//      "borrow-bearing | §9.7 | records (and inline arrays, §15.1) carrying
//      at least one borrow of a non-runtime-managed reference type"; and
//      holds-borrows: "only a record, an inline array (§15.1.2) or a choice can
//      hold borrows … On a record or an inline array it coincides with
//      **borrow-bearing**".
//
// So the NEWER revision governs: the v0.350/v0.355 §8.6 / §15.1.2 / §9.7 text
// supersedes the v0.315 reading the stale `#5659` comment cites.
//
// THE FIX. The escape disjunct is removed from the guard. Nothing else is
// read: the classification stays §3.6's / §15.1.2's own
// (`common::isBorrowBearingRecordOrSpan`, whose `#5627` inline-array leg
// already classifies `[N]Cls` and `[N]Pair` as borrow-bearing), and the ONE
// admitted position — the receiver of a member call, and an
// `IReadOnlySpan<TElement>`-declared position — is untouched.
//
// THE ACCEPT-CONTROLS ARE THE POINT. The escape was not only wrong for the
// borrow-bearing arrays: it was the ONLY thing that made the inline array the
// one value kind that could be simultaneously "borrow-bearing" and "not
// rejected". Removing it must NOT reject an array that holds no borrows —
// `[2]int` (§3.6: runtime-managed/primitive elements carry no borrow), and
// `[2]string` (§3.6: "runtime-managed values have unbounded lifetime by
// construction") — nor the RECORD twin's non-borrow-bearing form, nor the
// receiver of a member call (§8.6's one admitted position), nor an
// interpolation hole (§9.7 names it as the admitted spelling: "whose spelling
// is `span.ToString()` or an interpolation hole"). Each of those is pinned
// below.
//
// ROW 13 — the nullable ELEMENT. `[2]Cls?` is an inline array whose ELEMENT
// is nullable; the array itself is not a nullable type (§15.1: "Inline arrays
// themselves do not have a nullable form"), so nothing about it is a §7.1
// nullable-lifting question: §15.1.2 reads the element type "nullability
// stripped", so `[2]Cls?` is borrow-bearing exactly as `[2]Cls` is, and it is
// rejected by the §9.7 borrow-erasing bullet — NOT by a §7.1 rule. The
// assertion below pins that: the diagnostic must be the §9.7 one, and no
// §7.1 nullable diagnostic may appear.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// §9.7's single diagnostic, as it reads from EVERY route. The needle is the
// RULE, not a route: `checkBorrowErasingPosition` is one function called from
// all of them, so one phrase pins the rule and the `§9.7` citation pins the
// section it comes from.
bool rejectedBorrowErasing(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(
                "a value that holds borrows may not reach a "
                "borrow-erasing position") != std::string::npos &&
            d.message.find("\xC2\xA7""9.7") != std::string::npos &&
            d.message.find("borrow-bearing type") != std::string::npos)
            return true;
    return false;
}

// The same diagnostic, but insisting it NAMES the offending type — so a
// rejection at the right site with the wrong type named cannot pass.
bool rejectedBorrowErasingNaming(const JitResult& r, const std::string& type) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(
                "a value that holds borrows may not reach a "
                "borrow-erasing position") != std::string::npos &&
            d.message.find("has type `" + type + "`") != std::string::npos)
            return true;
    return false;
}

// True iff SOME error mentions `sub`. Used for the CONTROL rows, which pin a
// DIFFERENT rule (the §8.6 move-only one) and must not be satisfied by the
// §9.7 diagnostic.
bool hasErrorMentioning(const JitResult& r, const std::string& sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(sub) != std::string::npos)
            return true;
    return false;
}

// A shell with a `Cls` class and a borrow-bearing `[2]Cls` local. The two
// elements are `using var` bindings so their sources outlive the array, and
// the program carries no OTHER rule's violation.
std::string progWithInlineArray(const std::string& body) {
    return "module Conf; "
           "class Cls public constructor() end constructor; end Cls; "
           "class P "
           "  public static method Main(): int "
           "    using var k1 := new Cls(); using var k2 := new Cls(); "
           "    var m: [2]Cls := [k1, k2]; "
           "    " + body +
           "    return 0; "
           "  end Main; "
           "end P;";
}

// The RECORD TWIN — identical borrows, no inline array. §9.7's rule must
// reject this one exactly as it rejects the inline array.
std::string progWithRecordTwin(const std::string& body) {
    return "module Conf; "
           "class Cls public constructor() end constructor; end Cls; "
           "record Pair(A: Cls, B: Cls); "
           "class P "
           "  public static method Main(): int "
           "    using var k1 := new Cls(); using var k2 := new Cls(); "
           "    var p: Pair := new Pair(k1, k2); "
           "    " + body +
           "    return 0; "
           "  end Main; "
           "end P;";
}

}  // namespace

// =====================================================================
// ROW 1 — the issue's own repro: `Console.WriteLine(m)` over `m: [2]Cls`.
// =====================================================================
BEZEL_TEST(sema_borrow_erasing_inline_array_7852,
           inline_array_at_console_writeline_rejected_spec_9_7) {
    JitResult r = runAot(progWithInlineArray("Console.WriteLine(m);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBorrowErasing(r));
    // The diagnostic names the offending type: `[2]Cls`.
    BEZEL_EXPECT_TRUE(rejectedBorrowErasingNaming(r, "[2]Cls"));
}

// =====================================================================
// ROW 2 — the `TextWriter` receiver spelling (`w.WriteLine(m)`), which is
// the SAME `Object` parameter reached through a different arm (#7713).
// =====================================================================
BEZEL_TEST(sema_borrow_erasing_inline_array_7852,
           inline_array_at_textwriter_writeline_rejected_spec_9_7) {
    JitResult r = runAot(progWithInlineArray(
        "var w: TextWriter := Console.Out; w.WriteLine(m);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBorrowErasing(r));
    BEZEL_EXPECT_TRUE(rejectedBorrowErasingNaming(r, "[2]Cls"));
}

// =====================================================================
// ROW 8 — a LOCAL declared `Object`. §9.7's position list names "a local".
// =====================================================================
BEZEL_TEST(sema_borrow_erasing_inline_array_7852,
           inline_array_at_object_local_rejected_spec_9_7) {
    JitResult r = runAot(progWithInlineArray("var o: Object := m;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBorrowErasing(r));
    BEZEL_EXPECT_TRUE(rejectedBorrowErasingNaming(r, "[2]Cls"));
}

// =====================================================================
// ROW 9 — a USER method's `Object` parameter.
//
// The user's own `Sink.Keep(x: Object)` is the ordinary borrow-erasing
// parameter position; the record twin is rejected at the character-identical
// call (row 3b below), so the inline array must be too.
// =====================================================================
BEZEL_TEST(sema_borrow_erasing_inline_array_7852,
           inline_array_at_object_parameter_rejected_spec_9_7) {
    JitResult r = runAot(
        "module Conf; "
        "class Cls public constructor() end constructor; end Cls; "
        "class Sink public static method Keep(x: Object) end Keep; end Sink; "
        "class P "
        "  public static method Main(): int "
        "    using var k1 := new Cls(); using var k2 := new Cls(); "
        "    var m: [2]Cls := [k1, k2]; "
        "    Sink.Keep(m); "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBorrowErasing(r));
    BEZEL_EXPECT_TRUE(rejectedBorrowErasingNaming(r, "[2]Cls"));
}

// =====================================================================
// ROW 10 — `[2]Pair`, where `Pair` is a BORROW-BEARING RECORD: the
// `#5627` inline-array-of-borrow-bearing-record case.
//
// §15.1.2: an inline array is borrow-bearing "when its element type,
// nullability stripped, is … a borrow-bearing record".
// =====================================================================
BEZEL_TEST(sema_borrow_erasing_inline_array_7852,
           inline_array_of_borrow_bearing_record_rejected_spec_9_7) {
    JitResult r = runAot(
        "module Conf; "
        "class Cls public constructor() end constructor; end Cls; "
        "record Pair(A: Cls, B: Cls); "
        "class P "
        "  public static method Main(): int "
        "    using var k1 := new Cls(); using var k2 := new Cls(); "
        "    var m: [2]Pair := [new Pair(k1, k2), new Pair(k1, k2)]; "
        "    Console.WriteLine(m); "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBorrowErasing(r));
    BEZEL_EXPECT_TRUE(rejectedBorrowErasingNaming(r, "[2]Pair"));
}

// =====================================================================
// ROW 13 — the NULLABLE ELEMENT: `[2]Cls?`.
//
// The array itself is NOT a nullable type (§15.1: "Inline arrays themselves
// do not have a nullable form"), so this is NOT a §7.1 nullable-lifting
// question: §15.1.2 reads the element type "nullability stripped", so the
// array is borrow-bearing exactly as `[2]Cls` is, and the rejection is §9.7's
// borrow-erasing bullet. The assertions pin that: the §9.7 diagnostic is
// present, and NO §7.1 nullable diagnostic is reported alongside it.
// =====================================================================
BEZEL_TEST(sema_borrow_erasing_inline_array_7852,
           inline_array_with_nullable_element_rejected_by_9_7_not_7_1) {
    JitResult r = runAot(
        "module Conf; "
        "class Cls public constructor() end constructor; end Cls; "
        "class P "
        "  public static method Main(): int "
        "    using var k1 := new Cls(); using var k2 := new Cls(); "
        "    var m: [2]Cls? := [k1, k2]; "
        "    Console.WriteLine(m); "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBorrowErasing(r));
    // NOT a §7.1 nullable-lifting rejection: the array is not a nullable
    // type, so no nullable-to-non-nullable conversion is involved.
    BEZEL_EXPECT_FALSE(hasErrorMentioning(r, "\xC2\xA7""7.1"));
    BEZEL_EXPECT_FALSE(hasErrorMentioning(r, "nullable"));
}

// =====================================================================
// ROW 7b — a COLLECTION SLOT (`xs.Add(m)` at `xs: Array<Object>`).
//
// §9.7's position list names "a collection slot or collection-literal
// element"; the record twin is rejected at the character-identical call.
// =====================================================================
BEZEL_TEST(sema_borrow_erasing_inline_array_7852,
           inline_array_at_collection_slot_rejected_spec_9_7) {
    JitResult r = runAot(progWithInlineArray(
        "using var xs := new Array<Object>(); xs.Add(m);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBorrowErasing(r));
    BEZEL_EXPECT_TRUE(rejectedBorrowErasingNaming(r, "[2]Cls"));
}

// =====================================================================
// ROW 7c — a CHOICE PAYLOAD (`Boxed.One(m)` at a payload declared
// `Object`).
//
// §9.7's position list names "a choice payload". BEFORE the fix this row did
// not merely compile — it reached codegen and ABORTED the backend with
// "Invalid InsertValueInst operands! … insertvalue { ptr } undef, [2 x ptr]
// %18, 0" and "IR verification failed (malformed IR emitted)". The semantic
// rejection is what the rule asks for; the malformed IR never gets emitted.
// =====================================================================
BEZEL_TEST(sema_borrow_erasing_inline_array_7852,
           inline_array_at_choice_payload_rejected_spec_9_7) {
    JitResult r = runAot(
        "module Conf; "
        "class Cls public constructor() end constructor; end Cls; "
        "public choice Boxed One(v: Object); end Boxed; "
        "class P "
        "  public static method Main(): int "
        "    using var k1 := new Cls(); using var k2 := new Cls(); "
        "    var m: [2]Cls := [k1, k2]; "
        "    var c: Boxed := Boxed.One(m); "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBorrowErasing(r));
    BEZEL_EXPECT_TRUE(rejectedBorrowErasingNaming(r, "[2]Cls"));
}

// =====================================================================
// ROW 3 — the RECORD TWIN: `record Pair(A: Cls, B: Cls)` at
// `Console.WriteLine(p)`. UNCHANGED by this fix — pinned so a narrowing
// that broke the record twin would be caught.
// =====================================================================
BEZEL_TEST(sema_borrow_erasing_inline_array_7852,
           record_twin_at_console_writeline_still_rejected_spec_9_7) {
    JitResult r = runAot(progWithRecordTwin("Console.WriteLine(p);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBorrowErasing(r));
    BEZEL_EXPECT_TRUE(rejectedBorrowErasingNaming(r, "Pair"));
}

// ROW 3b — the record twin at a USER `Object` parameter: unchanged too.
BEZEL_TEST(sema_borrow_erasing_inline_array_7852,
           record_twin_at_object_parameter_still_rejected_spec_9_7) {
    JitResult r = runAot(
        "module Conf; "
        "class Cls public constructor() end constructor; end Cls; "
        "record Pair(A: Cls, B: Cls); "
        "class Sink public static method Keep(x: Object) end Keep; end Sink; "
        "class P "
        "  public static method Main(): int "
        "    using var k1 := new Cls(); using var k2 := new Cls(); "
        "    var p: Pair := new Pair(k1, k2); "
        "    Sink.Keep(p); "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBorrowErasing(r));
    BEZEL_EXPECT_TRUE(rejectedBorrowErasingNaming(r, "Pair"));
}

// =====================================================================
// ROW 4 — `[2]int`: a NON-borrow-bearing inline array boxes per the
// general rule (§8.6). MUST STAY ADMITTED.
// =====================================================================
BEZEL_TEST(sema_borrow_erasing_inline_array_7852,
           inline_array_of_int_at_console_writeline_still_accepted) {
    JitResult r = runAot(
        "module Conf; "
        "class P "
        "  public static method Main(): int "
        "    var m: [2]int := [1, 2]; "
        "    Console.WriteLine(m); "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedBorrowErasing(r));
}

// =====================================================================
// ROW 5 — `[2]string`: runtime-managed elements carry NO borrow (§3.6:
// "runtime-managed values have unbounded lifetime by construction"). MUST
// STAY ADMITTED.
// =====================================================================
BEZEL_TEST(sema_borrow_erasing_inline_array_7852,
           inline_array_of_string_at_console_writeline_still_accepted) {
    JitResult r = runAot(
        "module Conf; "
        "class P "
        "  public static method Main(): int "
        "    var m: [2]string := [\"a\", \"b\"]; "
        "    Console.WriteLine(m); "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedBorrowErasing(r));
}

// =====================================================================
// ROW 6 — `record Pair(A: string, B: string)`: a non-borrow-bearing
// record. MUST STAY ADMITTED.
// =====================================================================
BEZEL_TEST(sema_borrow_erasing_inline_array_7852,
           record_of_string_at_console_writeline_still_accepted) {
    JitResult r = runAot(
        "module Conf; "
        "record Pair(A: string, B: string); "
        "class P "
        "  public static method Main(): int "
        "    var p: Pair := new Pair(\"a\", \"b\"); "
        "    Console.WriteLine(p); "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedBorrowErasing(r));
}

// =====================================================================
// ROW 7 — `[2]Cls` as the RECEIVER of a member call (`m.ToString()`):
// §8.6's ONE admitted position. MUST STAY ADMITTED.
// =====================================================================
BEZEL_TEST(sema_borrow_erasing_inline_array_7852,
           inline_array_as_receiver_still_accepted_spec_8_6) {
    JitResult r = runAot(progWithInlineArray(
        "Console.WriteLine(m.ToString());"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedBorrowErasing(r));
}

// =====================================================================
// ROW 7b — an INTERPOLATION HOLE (`$"{m}"`): §9.7 names it as the admitted
// spelling ("whose spelling is `span.ToString()` or an interpolation hole").
// MUST STAY ADMITTED.
// =====================================================================
BEZEL_TEST(sema_borrow_erasing_inline_array_7852,
           inline_array_at_interpolation_hole_still_accepted_spec_9_7) {
    JitResult r = runAot(progWithInlineArray(
        "Console.WriteLine($\"{m}\");"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(rejectedBorrowErasing(r));
}

// =====================================================================
// ROW 11 — `Span<int>` at `Console.WriteLine(s)`: the `#5659` pinned case
// (corpus rows `s8_6_0007`, `s9_7_0027`). UNCHANGED — still rejected, with
// the same §9.7 diagnostic.
// =====================================================================
BEZEL_TEST(sema_borrow_erasing_inline_array_7852,
           span_at_console_writeline_still_rejected_spec_9_7) {
    JitResult r = runAot(
        "module Conf; "
        "class P "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<int>(4); "
        "    var s := buf.AsSpan(); "
        "    Console.WriteLine(s); "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(rejectedBorrowErasing(r));
    BEZEL_EXPECT_TRUE(rejectedBorrowErasingNaming(r, "Span"));
}

// =====================================================================
// ROW 12 — `[2 owned]Cls` at `Console.WriteLine(m)`: move-only, "cannot be
// boxed" (§8.6, #7694). A DIFFERENT rule, and the diagnostic must NOT
// change: the §9.7 borrow-erasing message must NOT appear, and the move-only
// one must.
// =====================================================================
BEZEL_TEST(sema_borrow_erasing_inline_array_7852,
           owned_inline_array_at_console_writeline_still_move_only_rejected) {
    JitResult r = runAot(
        "module Conf; "
        "class Cls public constructor() end constructor; end Cls; "
        "class P "
        "  public static method Main(): int "
        "    var m: [2 owned]Cls := [new Cls(), new Cls()]; "
        "    Console.WriteLine(m); "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_FALSE(r.ran);
    // The §8.6 move-only diagnostic (#7694) is the one that fires …
    BEZEL_EXPECT_TRUE(hasErrorMentioning(
        r, "a move-only value type, which \xC2\xA7""8.6 says cannot be boxed"));
    BEZEL_EXPECT_TRUE(hasErrorMentioning(r, "[2 owned]Cls"));
    // … and the §9.7 borrow-erasing one must NOT: an owning inline array is
    // not borrow-bearing, and this fix does not reach it.
    BEZEL_EXPECT_FALSE(rejectedBorrowErasing(r));
}

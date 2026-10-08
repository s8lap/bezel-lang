// Ownership — fix #4811: the §9.7 return-site check must read the RECEIVER of a
// member-access call, not only the call's ARGUMENTS.
//
// Spec (revision v0.330):
//   §9.7, borrow-lifetime bullet list — "A borrow-bearing record may be
//   **returned** only when every borrow it carries has a source the **caller
//   re-establishes at the call site** [K2.4]; a return carrying any other borrow
//   is rejected at the return site. Rejected at the return site, therefore: a
//   borrow of a scope-bound local, of a `using var`, of an `owned` parameter,
//   of an unbound temporary … and — … — of a **plain parameter** of the
//   returning method."
//   §9.7 admitted-return form 1 (receiver-sourced) — "a borrow of `this`, of a
//   bare read of one of the receiver's own fields … This is the **span shape**:
//   `public method ScanLine(y: int): Span<Pixel> => new Span<Pixel>(_pixels, y *
//   _width, _width);` over a `private owned _pixels: OwningBuffer<Pixel>` field
//   is admitted" (stdlib §4.14's `FrameBuffer`).
//   §4.14 / §4.15 — `Span<T>`/`ReadOnlySpan<T>` view an `OwningBuffer<T>`;
//   `ArraySpan<T>`/`ReadOnlyArraySpan<T>` view an `Array<T>`. A span is a BORROW
//   of that backing store, so it may not outlive it.
//
// THE DEFECT. `checkRecordBorrowEscapesOnReturn` ends its CallExpr arm with an
// unconditional `return`, and before this fix that arm walked only `call->args`.
// In `return local.AsSpan()` the borrow rides the RECEIVER and the argument list
// is empty, so the whole expression was ADMITTED and the returned view dangled
// once `local` was disposed at scope end. The receiver-sourced arm of §9.7
// form 1 is unreachable from here: the projection branch below the CallExpr arm
// is dead for any CallExpr, because the CallExpr arm always returns first.
//
// Each rejection row below is the issue's own shape and each FAILED pre-fix
// (the program compiled clean). The acceptance rows are the neighbours that pin
// the boundary this fix must NOT cross — they pass both before and after, and
// are what a too-broad receiver walk would break.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;

namespace {

// §9.7's own remedy clause, spelled once. Both the scope-bound-local and the
// parameter rejections of this check end in it.
const char* kRemedy =
    "bind the source in the caller's scope and return the span from there";
const char* kSpanCitation = "a span (§4.14/§4.15) is a borrow of its backing";

// A source local disposed at scope end, read off a member call. `owned:` selects
// the §4.14 (buffer-backed) or §4.15 (array-backed) family by the return type.
std::string program(const std::string& decl, const std::string& ret,
                    const std::string& body) {
    return "module M; class C "
           "  public static method M2(): " + ret + " " + decl + " " + body +
           " end M2; end C;";
}

}  // namespace

// THE DEFECT, buffer-backed (§4.14). `buf` is a `using var` — an owned local
// disposed when this method's scope ends — and `buf.AsSpan()` returns a view of
// it. Compiled clean before this fix.
BEZEL_TEST(record_borrow_escape_receiver_4811,
           member_call_receiver_using_var_buffer_rejected_9_7) {
    auto r = parseAndCheckOwnership(program(
        "using var buf := new OwningBuffer<byte>(64); ",
        "Span<byte>", "return buf.AsSpan(); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "returned span views `buf`"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kSpanCitation));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kRemedy));
}

// The same defect array-backed (§4.15), so the row covers the second span
// family rather than re-testing the first. `xs` is disposed at scope end.
BEZEL_TEST(record_borrow_escape_receiver_4811,
           member_call_receiver_using_var_array_rejected_9_7) {
    auto r = parseAndCheckOwnership(program(
        "using var xs := new Array<int>(); xs.Add(7); ",
        "ArraySpan<int>", "return xs.AsSpan(); "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "returned span views `xs`"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kSpanCitation));
}

// A PLAIN PARAMETER receiver. §9.7's closing text names this case explicitly
// and gives the reason the record return differs from a reference return: "a
// record type announces no source set at all, so a caller has nothing but the
// receiver to re-establish". The receiver walk is what reaches it — the argument
// list of `s.Slice(1)` is `(1)`, a literal.
BEZEL_TEST(record_borrow_escape_receiver_4811,
           member_call_receiver_plain_parameter_rejected_9_7) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Trim(s: ReadOnlySpan<byte>): ReadOnlySpan<byte> "
        "    return s.Slice(1); "
        "  end Trim; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasErrorMessage(r, "returned span is rooted at parameter `s`"));
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, kSpanCitation));
}

// The `owned`-parameter spelling of the same §9.7 clause ("of an `owned`
// parameter"). A distinct symbol kind from the plain parameter above, so it
// pins that the walk reaches both spellings of a method parameter.
BEZEL_TEST(record_borrow_escape_receiver_4811,
           member_call_receiver_owned_parameter_rejected_9_7) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method V(owned buf: OwningBuffer<byte>): Span<byte> "
        "    return buf.AsSpan(); "
        "  end V; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(
        hasErrorMessage(r, "returned span is rooted at parameter `buf`"));
}

// ── ACCEPTANCE ROWS ─────────────────────────────────────────────────────────
// These pass before AND after. They are the boundary: a receiver walk that did
// not stop at the §9.7 sources would reject each of them.

// §9.7 admitted-return form 1 verbatim — stdlib §4.14's `FrameBuffer.ScanLine`.
// The receiver is `this`; the borrow is of the receiver's own `owned` field,
// which outlives the frame. The receiver walk must read `this` (an
// IdentifierExpr sema leaves unresolved), see neither a parameter nor an
// escape-bound local, and fall through.
BEZEL_TEST(record_borrow_escape_receiver_4811,
           receiver_sourced_field_span_accepted_9_7) {
    auto r = parseAndCheckOwnership(
        "module M; record Pixel(x: int); class FrameBuffer "
        "  private owned _pixels: OwningBuffer<Pixel>; "
        "  private readonly _width: int; "
        "  public constructor(n: int) _pixels := new OwningBuffer<Pixel>(n); "
        "    _width := n; end constructor; "
        "  public method ScanLine(y: int): Span<Pixel> => "
        "    _pixels.AsSpan(y * _width, _width); "
        "end FrameBuffer;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A borrow-bearing RECORD returned from an instance method, receiver-sourced —
// the form §9.7 admits for a user record, and the one the projection branch's
// `!typeIsSpan` guard exists to keep admitted. If the receiver walk were gated
// on the span family instead of §9.7's source classes, this would flip.
BEZEL_TEST(record_borrow_escape_receiver_4811,
           receiver_sourced_user_record_accepted_9_7) {
    auto r = parseAndCheckOwnership(
        "module M; class Item "
        "  public v: int; "
        "  public constructor(x: int) v := x; end constructor; "
        "end Item; "
        "record Holder(S: Item?); class C "
        "  private owned _item: Item; "
        "  public constructor() _item := new Item(1); end constructor; "
        "  public method Make(): Holder => new Holder(_item); "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A return that borrows nothing at all. The check is gated on
// `isBorrowBearingRecordOrSpan` before the receiver is read, so a value-typed
// result off an owned local receiver must be untouched.
BEZEL_TEST(record_borrow_escape_receiver_4811,
           non_borrowing_return_over_owned_local_accepted_9_7) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  public static method Count(): int "
        "    using var xs := new Array<int>(); xs.Add(1); xs.Add(2); "
        "    return xs.Count; "
        "  end Count; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A call whose borrow rides an ARGUMENT, not the receiver: the receiver is a
// `this` field (neither a parameter nor escape-bound) and the argument is an
// escape-bound local. The receiver walk must FALL THROUGH to the argument scan
// that already existed, not return on its own account — the failure mode this
// run has seen twice, where a new `return` on an existing dispatch silently
// removes every rule the old leg enforced.
BEZEL_TEST(record_borrow_escape_receiver_4811,
           argument_sourced_borrow_still_rejected_after_receiver_walk_9_7) {
    auto r = parseAndCheckOwnership(
        "module M; class Item "
        "  public v: int; "
        "  public constructor(x: int) v := x; end constructor; "
        "end Item; "
        "record Holder(S: Item?); class C "
        "  public method Build(s: Item?): Holder => new Holder(s); "
        "  public method Leak(): Holder "
        "    using var local := new Item(3); "
        "    return this.Build(local); "
        "  end Leak; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The call-site diagnostic, naming the ARGUMENT `local`. The receiver is
    // `this`, so the receiver walk above must have fallen through to the
    // argument scan for this row to exist at all.
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "borrow of `local` whose source is disposed at scope end"));
}
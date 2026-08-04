// Sema — issue #2311 / stdlib §4.14 (`Span<T>` / `ReadOnlySpan<T>`), §4.15
// (`ArraySpan<T>` / `ReadOnlyArraySpan<T>`), §4.16 (`IReadOnlySpan<T>`), §3.3
// (`ReadOnlyStringSpan`); language spec §8.5 (`ToString`), §8.6 (Boxing of
// borrow-bearing records), v0.236.
//
// `ToString()` on the span family was an ACCEPT-TRAP with TWO distinct
// failure modes, both verified mechanically on e87d868 (plain, non-ASan build
// unless noted):
//
//   1. CONCRETE receiver (`Span<T>` etc.) — sema typed the call `string` via
//      the universal-`ToString` rule, but no span member block declares or
//      lowers it, so no value was produced:
//        * `var t := sp.ToString(); Console.WriteLine(t);` printed NOTHING —
//          not even a newline — with only an `internal:` #2434 note; exit 0.
//        * `Console.WriteLine(sp.ToString());` printed nothing and emitted NO
//          diagnostic at all; exit 0. The whole statement was dropped.
//
//   2. INTERFACE receiver (`IReadOnlySpan<T>`) — the memory-safety half, and
//      the mechanism the issue originally reported. A span passed to an
//      `IReadOnlySpan<T>` parameter (§8.6: "The canonical sanctioned use is
//      passing a span to a parameter of type `IReadOnlySpan<TElement>`") takes
//      the `isIfaceRecv` branch, is typed `string`, and reaches REAL VTABLE
//      DISPATCH on a `{ptr,i32,i32}` struct value that has no vtable:
//            in F, len=3
//            Segmentation fault (exit 139)
//        under ASan: "SEGV on unknown address 0x000000000000 (pc
//        0x000000000000)" / "Hint: pc points to the zero page" — a wild
//        indirect jump. This path is NOT exotic: §8.6 blesses it explicitly.
//
// WHY A DEFERRAL DIAGNOSTIC AND NOT A REJECTION, AND NOT A LOWERING.
//
// An earlier version of this fix REJECTED `ToString` outright ("no member
// `ToString` on `ReadOnlySpan<T>`") on the grounds that §4.14/§4.15 declare
// none. That was sent back for over-committing, and the reasons are recorded
// here so it is not re-attempted:
//   * §4.14 states outright that "`Span<T>` is purely user-level Bezel — no
//     compiler intrinsic. The record machinery, borrow lifetime tracking, and
//     indexer dispatch are all standard language features." §8.5's default
//     `ToString` for positional records IS record machinery, and §4.14 declares
//     `public record Span<T>(Buffer: OwningBuffer<T>, Start: int, Length: int)`
//     — exactly the shape §8.5 gives "a representation of fields".
//   * Absence from the §4.14 record body proves nothing: NO stdlib record body
//     lists its compiler-generated members. That same body declares no
//     `Equals`/`GetHashCode` either, and those are auto-generated.
//   * §4.14's `ToArray()` remark ("the role `ToString()` plays on
//     `ReadOnlyStringSpan`") is about MATERIALIZATION — span content to a value
//     — not about §8.5's debug field-dump. Different operations.
// Rejecting would have made the compiler treat differently two things the spec
// describes identically: a user positional record (see
// `control_user_record_tostring_works`) and `Span<T>`.
//
// Implementing the §8.5 default instead would settle the same open question in
// the other direction. So this follows the in-repo #511 `GetType` precedent
// (ExprTypecheck_Calls.cpp): report NOT YET IMPLEMENTED while still TYPING the
// expression `string`. That closes the actual bug — the silent wrong value and
// the wild jump — without ruling; typing it prevents secondary "untyped
// expression" cascades and preserves the future implementation path. The
// §8.5-vs-§4.14 question is tracked in #2661.
//
// Per tests/README.md the coarse `rejects()` helper cannot distinguish a
// rejection from a runtime SIGSEGV — which matters more than usual here, since
// case 2 genuinely DID segfault. Every test below asserts the diagnostic TEXT.
//
// Mechanical verification: reverting the `isDeferredSpanRecv` block in
// src/sema/ExprTypecheck_Calls.cpp leaves the concrete cases compiling silently
// (exit 0, no diagnostic) and the interface cases SEGFAULTING, so each fails;
// the four accept controls pass either way.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

bool aotWasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string_view::npos) return true;
    return false;
}

// The discriminator. Asserts the deferral TEXT, never `!r.ran` — which here
// would also be satisfied by the very SIGSEGV this fix removes.
bool deferredWithReason(const std::string& program) {
    JitResult r = runAot(program);
    if (r.ran) return false;
    bool sawNotImpl = false, sawTracking = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("`ToString()` on") != std::string_view::npos &&
            d.message.find("is not yet implemented") != std::string_view::npos)
            sawNotImpl = true;
        if (d.message.find("#2661") != std::string_view::npos)
            sawTracking = true;
    }
    return sawNotImpl && sawTracking;
}

// §4.14 source: `OwningBuffer<int>` -> Span<T> / ReadOnlySpan<T>.
std::string buf3(const std::string& body) {
    return "module M; import System.Collections.Generic; "
           "class C public static method Main(): int "
           "  using var buf := new OwningBuffer<int>(3); "
           "  buf[0] := 10; buf[1] := 11; buf[2] := 12; "
           + body +
           "end Main; end C;";
}

// §4.15 source: `Array<int>` -> ArraySpan<T> / ReadOnlyArraySpan<T>.
std::string arr3(const std::string& body) {
    return "module M; import System.Collections.Generic; "
           "class C public static method Main(): int "
           "  using var arr := new Array<int>(3, 0); "
           + body +
           "end Main; end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// §4.14 — the issue's own repro, plus the writable sibling.
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_span_tostring_2311, readonlyspan_tostring_deferred) {
    BEZEL_EXPECT_TRUE(deferredWithReason(buf3(
        "  var sp := buf.AsReadOnlySpan(); "
        "  var t := sp.ToString(); "
        "  Console.WriteLine(t); "
        "  return 0; ")));
}

BEZEL_TEST(sema_span_tostring_2311, span_tostring_deferred) {
    BEZEL_EXPECT_TRUE(deferredWithReason(buf3(
        "  var sp := buf.AsSpan(); "
        "  var t := sp.ToString(); "
        "  Console.WriteLine(t); "
        "  return 0; ")));
}

// The statement-position shape, which pre-fix emitted NO diagnostic at all
// (not even the #2434 note) and silently dropped the whole `Console.WriteLine`.
BEZEL_TEST(sema_span_tostring_2311, span_tostring_statement_position_deferred) {
    BEZEL_EXPECT_TRUE(deferredWithReason(buf3(
        "  var sp := buf.AsReadOnlySpan(); "
        "  Console.WriteLine(sp.ToString()); "
        "  return 0; ")));
}

// ---------------------------------------------------------------------------
// §4.15 — the array-backed pair.
// ---------------------------------------------------------------------------

BEZEL_TEST(sema_span_tostring_2311, arrayspan_tostring_deferred) {
    BEZEL_EXPECT_TRUE(deferredWithReason(arr3(
        "  var sp := arr.AsSpan(); "
        "  var t := sp.ToString(); "
        "  Console.WriteLine(t); "
        "  return 0; ")));
}

BEZEL_TEST(sema_span_tostring_2311, readonly_arrayspan_tostring_deferred) {
    BEZEL_EXPECT_TRUE(deferredWithReason(arr3(
        "  var sp := arr.AsReadOnlySpan(); "
        "  var t := sp.ToString(); "
        "  Console.WriteLine(t); "
        "  return 0; ")));
}

// ---------------------------------------------------------------------------
// §4.16 / §8.6 — THE MEMORY-SAFETY HALF. These are the tests that matter most:
// pre-fix each of them SEGFAULTED (exit 139, ASan "pc points to the zero
// page"), because the interface receiver reaches genuine vtable dispatch on a
// struct value. An earlier draft of this fix covered only the concrete
// receivers and left both of these crashing.
// ---------------------------------------------------------------------------

// §8.6: "The canonical sanctioned use is passing a span to a parameter of type
// `IReadOnlySpan<TElement>`" — the ordinary, blessed way to reach this.
BEZEL_TEST(sema_span_tostring_2311, ireadonlyspan_param_tostring_deferred) {
    BEZEL_EXPECT_TRUE(deferredWithReason(
        "module M; import System.Collections.Generic; "
        "class C "
        "  private static method F(s: IReadOnlySpan<int>): int "
        "    var t := s.ToString(); "
        "    Console.WriteLine(t); "
        "    return 0; "
        "  end F; "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<int>(3); "
        "    buf[0] := 10; buf[1] := 11; buf[2] := 12; "
        "    var sp := buf.AsReadOnlySpan(); "
        "    return F(sp); "
        "  end Main; "
        "end C;"));
}

// The interface-typed LOCAL, reached by the implicit span -> IReadOnlySpan
// coercion rather than by a call. Same dispatch, different syntax.
BEZEL_TEST(sema_span_tostring_2311, ireadonlyspan_local_tostring_deferred) {
    BEZEL_EXPECT_TRUE(deferredWithReason(buf3(
        "  var sp := buf.AsReadOnlySpan(); "
        "  var i: IReadOnlySpan<int> := sp; "
        "  Console.WriteLine(i.ToString()); "
        "  return 0; ")));
}

// ---------------------------------------------------------------------------
// ACCEPT controls — the false-reject guards. The deferral keys on a head name
// shared with receivers that legitimately answer the same structural tests, so
// each must keep working. All assert MATERIALIZED TEXT, not just compiling.
// ---------------------------------------------------------------------------

// stdlib §3.3 DOES declare `ToString(): string` on `ReadOnlyStringSpan`
// ("Materialization"), fixed by #971/9036830. It must keep typing AND lowering.
BEZEL_TEST(sema_span_tostring_2311, control_readonlystringspan_tostring_works) {
    std::string out = runAotOut(
        "module M; class C public static method Main(): int "
        "  Console.WriteLine(\"hello\".AsSpan(1, 3).ToString()); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(out.find("ell") != std::string::npos);
}

// §8.5 / #608: a builtin exception type is also a Named TypeRef with no user
// TypeSymbol — the case the universal rule was written for. It carries no
// astNode, which is what keeps it out of the span carve-out.
BEZEL_TEST(sema_span_tostring_2311, control_builtin_exception_tostring_works) {
    // §8.5, "For classes: the type's qualified name".
    std::string out = runAotOut(
        "module M; class C public static method Main(): int "
        "  try "
        "    throw new ArgumentException(\"boom\"); "
        "  catch var e: Exception => Console.WriteLine(\"EXN[\" + e.ToString() + \"]\"); "
        "  end; "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(out.find("EXN[ArgumentException]") != std::string::npos);
}

// §8.5: a user positional record's default `ToString`. This is precisely the
// shape the deferral declines to rule on — `Span<T>` is declared the same way —
// so a regression here would mean the deferral had swallowed §8.5 generally.
BEZEL_TEST(sema_span_tostring_2311, control_user_record_tostring_works) {
    std::string out = runAotOut(
        "module M; "
        "public record Point(X: int, Y: int) end Point; "
        "class C public static method Main(): int "
        "  var p := new Point(10, 20); "
        "  Console.WriteLine(p.ToString()); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(out.find("Point(X: 10, Y: 20)") != std::string::npos);
}

// The rest of the documented span surface must be unaffected.
//
// Each result is bound to a LOCAL rather than tested through an inline guard.
// That is deliberate and was a review finding on the first draft: by this
// issue's own mechanism, a guard whose operand never lowers is a DROPPED guard,
// so an inline-guard version would still fall through to `return 33` and pass
// even if the whole surface regressed to non-lowering. Bound to locals, a
// non-lowering member degrades its slot to zero (#2434) and the comparison
// genuinely fails.
BEZEL_TEST(sema_span_tostring_2311, control_documented_span_surface_still_works) {
    JitResult r = runAot(buf3(
        "  var sp := buf.AsReadOnlySpan(); "
        "  var len := sp.Length; "
        "  var empty := sp.IsEmpty; "
        "  var idx := sp.IndexOf(11); "
        "  var has := sp.Contains(12); "
        "  var sl := sp.Slice(1); "
        "  var slLen := sl.Length; "
        "  if len <> 3 then return 1; end; "
        "  if empty then return 2; end; "
        "  if idx <> 1 then return 3; end; "
        "  if has = false then return 4; end; "
        "  if slLen <> 2 then return 5; end; "
        "  return 33; "));
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 33);
}

// The §4.16 interface's OWN documented surface must still work — the deferral
// must intercept `ToString` on an `IReadOnlySpan<T>` receiver without
// disturbing the members §4.16 does declare.
BEZEL_TEST(sema_span_tostring_2311, control_ireadonlyspan_surface_still_works) {
    JitResult r = runAot(
        "module M; import System.Collections.Generic; "
        "class C "
        "  private static method F(s: IReadOnlySpan<int>): int "
        "    var len := s.Length; "
        "    var first := s[0]; "
        "    if len <> 3 then return 1; end; "
        "    if first <> 10 then return 2; end; "
        "    return 44; "
        "  end F; "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<int>(3); "
        "    buf[0] := 10; buf[1] := 11; buf[2] := 12; "
        "    var sp := buf.AsReadOnlySpan(); "
        "    return F(sp); "
        "  end Main; "
        "end C;");
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 44);
}

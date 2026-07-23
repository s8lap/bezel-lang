// Spec-derived tests for issue #1966 — the INDIRECT span borrow-escape, the
// explicitly-deferred residual of #1959.
//
// §9.7 / §4.14 / §4.15: a span is a borrow of its backing Array / OwningBuffer
// and may not outlive it. #1959 closed the DIRECT projection forms
// (`return local.AsSpan();`, `return local[i]`, `return new ArraySpan<int>(
// local, …)`). The INDIRECT form — bind the span to a local first, then return
// the local — was scoped out because it requires tracking the span var's borrow
// provenance back to `local`:
//
//   public static method Leak(): ArraySpan<int>
//     var local := new Array<int>(); local.Add(7);
//     var s := local.AsSpan();   // span bound to a local (source: `local`)
//     return s;                  // `local` auto-drops at return -> dangling -> UAF
//   end Leak;
//
// The span var's provenance to `local` already lives in recordBorrowLocalSource_
// (collectRecordBorrowSource, the #1726 consume-freeze machinery), but
// checkIndirectEscapeOnReturn gated its carriedEscapeBoundSourceLiveAt query
// behind `!isBorrowBearingRecordOrSpan`, EXCLUDING exactly the span holders; the
// direct-return check (checkRecordBorrowEscapesOnReturn) consults only the
// returned expression's identifierRoot, not a span holder's carried source. So
// the span-var return compiled and SEGFAULTED at runtime.
//
// Fix: drop that exclusion — run the carried-source query for span / borrow-
// bearing-record holders too, with the §9.7 span/record diagnostic. The REJECT
// side (every leaky shape: one-hop, two-hop, slice-hop, read-only) is pinned by
// the lit tests (tests/lit/ownership/indirect_span_escape_1966__*). This file
// pins the ACCEPT side: the legal within-frame span uses must stay ASan-clean
// (no over-rejection). Each returns a distinct non-zero exit code so a silently
// wrong value is caught too.

#include <string>

#include "support/aot_support.hpp"       // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

// ===========================================================================
// ACCEPT — legal within-frame span uses (must NOT be over-rejected; ASan-clean).
// ===========================================================================

// §4.16 / §9.7: a span bound to a local, then PASSED DOWN as an argument to a
// synchronous callee. The call frame bounds the borrow; `local` outlives the
// call. Sum([7, 9]) == 16.
BEZEL_TEST(indirect_span_escape_1966, spanvar_passed_down_as_argument_accepted) {
    std::string src =
        "module M; class C "
        "  public static method Sum(sp: ArraySpan<int>): int "
        "    var total := 0; var i := 0; "
        "    while i < sp.Length do total := total + sp[i]; i := i + 1; end; "
        "    return total; "
        "  end Sum; "
        "  public static method Use(): int "
        "    var local := new Array<int>(); local.Add(7); local.Add(9); "
        "    var s := local.AsSpan(); "
        "    return C.Sum(s); "
        "  end Use; "
        "  public static method Main(): int return C.Use(); end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 16);
}

// §4.15 / §9.7: a derived view `var t := s.Slice(1, 2);` USED within the frame
// (not returned) is legal. s over [7, 9, 11]; t == [9, 11]; t[0] == 9.
BEZEL_TEST(indirect_span_escape_1966, spanvar_local_slice_used_in_frame_accepted) {
    std::string src =
        "module M; class C "
        "  public static method Use(): int "
        "    var local := new Array<int>(); "
        "    local.Add(7); local.Add(9); local.Add(11); "
        "    var s := local.AsSpan(); "
        "    var t := s.Slice(1, 2); "
        "    return t[0]; "
        "  end Use; "
        "  public static method Main(): int return C.Use(); end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 9);
}

// §4.15 / §9.7: a span var iterated in a LOOP within its own frame. sum == 16.
BEZEL_TEST(indirect_span_escape_1966, spanvar_loop_use_in_frame_accepted) {
    std::string src =
        "module M; class C "
        "  public static method Use(): int "
        "    var local := new Array<int>(); local.Add(7); local.Add(9); "
        "    var s := local.AsSpan(); "
        "    var total := 0; var i := 0; "
        "    while i < s.Length do total := total + s[i]; i := i + 1; end; "
        "    return total; "
        "  end Use; "
        "  public static method Main(): int return C.Use(); end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 16);
}

// §9.7 POSITIVE control: a span var sourced from a PARAM array, then returned.
// The parameter's array outlives this method's frame (the caller's array
// persists past the call), so `local`'s escape reasoning does NOT apply — the
// param root is not an owned scope-bound local, so the borrow is not escape-
// bound. Must stay accepted (the direct #1959 `return a.AsSpan()` sibling, now
// through a local). sp[0] == 5.
BEZEL_TEST(indirect_span_escape_1966, spanvar_of_param_array_returned_accepted) {
    std::string src =
        "module M; class C "
        "  public static method View(a: Array<int>): ArraySpan<int> "
        "    var s := a.AsSpan(); "
        "    return s; "
        "  end View; "
        "  public static method Main(): int "
        "    var arr := new Array<int>(); arr.Add(5); "
        "    var sp := C.View(arr); "
        "    return sp[0]; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// §9.7 POSITIVE control: a span var sourced from an OWNED FIELD of `this`, then
// returned. The field's storage outlives the call (the receiver persists past
// this instance method's exit); the receiver root `this` is not a tracked scope-
// bound local, so the borrow is not escape-bound. Must stay accepted (the
// indirect sibling of #1959's return_span_of_owned_field_clean). sp[0] == 8.
BEZEL_TEST(indirect_span_escape_1966, spanvar_of_owned_field_returned_accepted) {
    std::string src =
        "module M; class C "
        "  public owned _buf: Array<int> := new Array<int>(); "
        "  public method View(): ArraySpan<int> "
        "    var s := this._buf.AsSpan(); "
        "    return s; "
        "  end View; "
        "  public static method Main(): int "
        "    using var c := new C(); c._buf.Add(8); "
        "    var sp := c.View(); "
        "    return sp[0]; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 8);
}

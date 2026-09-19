// Spec-derived tests for issue #2127 — a record with a BUILTIN-SPAN field is
// now classified borrow-bearing (§9.7 transitivity, §4.14/§4.15).
//
// §9.7: "A record type is a borrow-bearing record ... if it holds at least one
// borrow-carrying field of a non-runtime-managed type. A field is borrow-
// carrying when: ... Its type is another borrow-bearing record ...
// (transitivity)." A builtin span view (`Span`/`ReadOnlySpan`, §4.14;
// `ArraySpan`/`ReadOnlyArraySpan`, §4.15) is a borrow of its backing
// Array/OwningBuffer — a borrow-bearing record without a user Record TypeDecl —
// so a `record Holder(S: ArraySpan<int>)` is borrow-bearing by transitivity.
//
// Before the fix, isBorrowBearingRecordImpl's field walk bailed on the span
// field (its `declaration` is null / value-builtin, and it is not a user
// Record), so `Holder` was classified NOT borrow-bearing. Every §9.7/§30.1
// escape check therefore skipped it and both the DIRECT (`return new
// Holder(local.AsSpan())`) and INDIRECT (`var h := new Holder(s); return h`)
// escape forms COMPILED and SEGFAULTED (dangling span read once `local`
// auto-drops). The REJECT side (every leaky shape) is pinned by the lit tests
// (tests/lit/ownership/record_span_field_escape_2127__* and
// tests/lit/sema/record_span_field_escape_2127__async_*). This file pins the
// ACCEPT side: the legal within-frame span-field-record uses must stay
// ASan-clean (no over-rejection) and produce the right value. Each returns a
// distinct exit code so a silently wrong value is caught too.
//
// // per §9.7, §4.14, §4.15, §30.1 (v0.220, #2127)

#include <string>

#include "support/aot_support.hpp"       // runAotChecked — AOT + ASan/LSan oracle
#include "support/ownership_support.hpp"  // parseAndCheckOwnership, hasErrorMessage (#4428 §9.7 v0.301)
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

// ===========================================================================
// ACCEPT — legal within-frame span-field-record uses (must NOT be over-
// rejected; ASan-clean). The source of every carried borrow outlives the
// record, so §9.7's borrow-lifetime rules are satisfied.
// ===========================================================================

// §9.7: a span-field record built and USED within its own frame — `local`
// outlives `h`, so reading `h.S[0]` is sound. == 7.
BEZEL_TEST(record_span_field_escape_2127, in_frame_use_accepted) {
    std::string src =
        "module M; record Holder(S: ArraySpan<int>); class C "
        "  public static method Main(): int "
        "    var local := new Array<int>(); local.Add(7); "
        "    var s := local.AsSpan(); "
        "    var h := new Holder(s); "
        "    return h.S[0]; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// §9.7: a span-field record PASSED DOWN as an argument to a synchronous callee.
// The call frame bounds the borrow; `local` outlives the call. == 16.
BEZEL_TEST(record_span_field_escape_2127, passed_down_as_argument_accepted) {
    std::string src =
        "module M; record Holder(S: ArraySpan<int>); class C "
        "  public static method Use(h: Holder): int "
        "    return h.S[0] + h.S[1]; "
        "  end Use; "
        "  public static method Main(): int "
        "    var local := new Array<int>(); local.Add(7); local.Add(9); "
        "    var h := new Holder(local.AsSpan()); "
        "    return C.Use(h); "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 16);
}

// §9.7: a DIRECT construction from a fresh span projection, used in-frame (the
// record is not returned; an int projection is). `local` outlives `h`. == 11.
BEZEL_TEST(record_span_field_escape_2127, direct_construct_in_frame_accepted) {
    std::string src =
        "module M; record Holder(S: ArraySpan<int>); class C "
        "  public static method Main(): int "
        "    var local := new Array<int>(); local.Add(11); "
        "    var h := new Holder(local.AsSpan()); "
        "    return h.S[0]; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 11);
}

// §9.7 transitivity: a NESTED span-field record (`Outer` holds `Holder` holds a
// span) used in-frame. Both are borrow-bearing; `local` outlives both. == 13.
BEZEL_TEST(record_span_field_escape_2127, nested_record_in_frame_accepted) {
    std::string src =
        "module M; record Holder(S: ArraySpan<int>); record Outer(H: Holder); "
        "class C "
        "  public static method Main(): int "
        "    var local := new Array<int>(); local.Add(13); "
        "    var h := new Holder(local.AsSpan()); "
        "    var o := new Outer(h); "
        "    return o.H.S[0]; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 13);
}

// §4.15 / §9.7: the ReadOnlyArraySpan family variant, in-frame. The name-head
// span predicate recognizes all four span families uniformly. == 21.
BEZEL_TEST(record_span_field_escape_2127, readonly_span_field_in_frame_accepted) {
    std::string src =
        "module M; record RoHolder(S: ReadOnlyArraySpan<int>); class C "
        "  public static method Main(): int "
        "    var local := new Array<int>(); local.Add(21); "
        "    var h := new RoHolder(local.AsReadOnlySpan()); "
        "    return h.S[0]; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 21);
}

// fix #4428 (§9.7, v0.301): a span-field record constructed DIRECTLY from a
// PARAM array's span, then returned — is REJECTED at the return site, naming
// §9.7 v0.301. The pre-fix text pinned the opposite verdict ("the parameter
// outlives this method's frame, so the carried source is NOT escape-bound
// and the return must stay accepted"); that was the soundness hole #4428 —
// the §9.7 v0.301 ruling closes the parameter arm: "a record type announces
// no source set at all, so a caller has nothing but the receiver to re-
// establish and a parameter-sourced record return would hand back a borrow
// no call site could bound". The S10 sibling below
// (param_sourced_spanvar_returned_accepted, the INDIRECT `var s := a.AsSpan();
// return new Holder(s);` form) stays accepted — its return is rooted at the
// span local `s`, not at the parameter `a`, and `s` carries the borrow's
// provenance through the §28.3 / §9.7 transitivity walk so the call-site
// freeze re-establishes the parameter-sourced borrow at the binding site.
// // per §9.7 v0.301; the callee-side rejection is reportIfParameterRoot.
BEZEL_TEST(record_span_field_escape_2127, param_sourced_direct_returned_rejected_9_7_v301) {
    std::string src =
        "module M; record Holder(S: ArraySpan<int>); class C "
        "  public static method View(a: Array<int>): Holder "
        "    return new Holder(a.AsSpan()); "
        "  end View; "
        "  public static method Main(): int "
        "    var arr := new Array<int>(); arr.Add(5); "
        "    var h := C.View(arr); "
        "    return h.S[0]; "
        "  end Main; "
        "end C;";
    auto r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "returned borrow-bearing record carries a borrow of parameter `a`"));
}

// §9.7 POSITIVE control: the same param-sourced return through an INTERMEDIATE
// span var (`var s := a.AsSpan(); return new Holder(s);`). The carried-source
// chain resolves `h`->`s`->`a` (a parameter, not escape-bound), so it stays
// accepted — the transitive-hop counterpart of the control above. == 6.
BEZEL_TEST(record_span_field_escape_2127, param_sourced_spanvar_returned_accepted) {
    std::string src =
        "module M; record Holder(S: ArraySpan<int>); class C "
        "  public static method View(a: Array<int>): Holder "
        "    var s := a.AsSpan(); "
        "    return new Holder(s); "
        "  end View; "
        "  public static method Main(): int "
        "    var arr := new Array<int>(); arr.Add(6); "
        "    var h := C.View(arr); "
        "    return h.S[0]; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 6);
}

// §9.7 NO-OVER-REJECTION control: a PLAIN value record (int fields only, never
// borrow-bearing) constructed and RETURNED — the reclassification must not
// sweep in a record with no span/borrow field. == 7.
BEZEL_TEST(record_span_field_escape_2127, plain_value_record_returned_accepted) {
    std::string src =
        "module M; record Point(X: int, Y: int); class C "
        "  public static method Make(): Point "
        "    var p := new Point(3, 4); "
        "    return p; "
        "  end Make; "
        "  public static method Main(): int "
        "    var p := C.Make(); "
        "    return p.X + p.Y; "
        "  end Main; "
        "end C;";
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

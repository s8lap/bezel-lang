// Spec-derived tests for issue #5847 — §9.7 (v0.334) the "one source set per
// named location" model applied to a borrow-bearing HOLDER, the third kind of
// return-site root the §9.7 projection check did not classify.
//
// §9.7's model of a value held at a named location: "A record value held at a
// named location — a local or parameter, a field, a slot of an inline array held
// at one — is read with **one source set** … a whole-value assignment gives the
// location the assigned value's set, and a **member store** … adds the sources of
// the borrows `e` carries to that set, **never removing one**, and is checked as
// an assignment of the updated value to the location that holds it" (spec §9.7,
// v0.334). §28.11's borrowing-collection store is the element-wise analogue of
// that member store: `spans.Add(buf.AsSpan())` puts a borrow of `buf` INTO the
// location `spans`, so `spans` thereafter reads with a source set containing
// `buf`, and a span read back out of `spans` carries it.
//
// The defect. `checkRecordBorrowEscapesOnReturn`'s projection branch classified
// the returned expression's receiver ROOT by what the root ITSELF is: a method
// parameter (#4428, §9.7 v0.301), or a tracked owned local (a scope-bound
// `using var` / owned local, whose own dispose is the problem). A THIRD kind of
// local matched neither — a borrow-bearing holder — and both checks fell
// through, so the projection was admitted even though the span's real source is
// disposed at this frame's scope end. That is a use-after-free, not a warning
// gap. The issue repro compiled rc=0 and ran SIGSEGV rc=139; under ASan,
// `SEGV on unknown address 0x00000000000d`, `READ memory access`, `#1 memcpy`.
//
// The root cause was not the missing CHECK but the missing WRITER:
// `collectRecordBorrowSource` — the only writer of the source set — fires solely
// from a LocalDecl / LocalConstDecl whose OWN type is a borrow-bearing record /
// span / choice. A collection local's own type (`Array<Span<byte>>`) is none of
// those, so the set stayed empty and `carriedEscapeBoundSourceLiveAt` found
// nothing through it. `collectCollectionBorrowStoreSource` is the missing
// writer; the resolution at the return site already existed and is consulted
// unchanged.
//
// THE DIFFERENTIAL CONTROL is the second row and it is what makes this a fix
// rather than a narrowing. The control is byte-identical to the repro except
// that the source is a PARAMETER (`LeakSpan(buf: OwningBuffer<byte>)`), whose
// storage the caller supplied and which therefore outlives this frame. It
// records the SAME source set — the fix records it in both cases — but
// `carriedEscapeBoundSourceLiveAt`'s `isEscapeBoundLocal` test is false for a
// parameter, so nothing is reported and the row stays accepted and runs to the
// correct value. If the fix rejected the control, it would be a narrowing: the
// escape-boundness test, not the recording, is what distinguishes the two.
//
// Rows:
//   1. REJECT — the issue's exact repro, asserting the SPECIFIC §9.7 diagnostic
//      text (not `failedAtCompileTimeWithRealLocation`, which passes on a crash
//      AND on a rejection for the wrong reason), plus its indexer-set twin and
//      the indirect one-hop form.
//   2. ACCEPT — the parameter control, AOT, asserting the correct exit code,
//      plus the receiver-sourced and value-element controls.
//
// Fail-without, measured mechanically per row by reverting both src files to
// the base SHA (md5 confirmed changed, the `.o` confirmed rebuilt): the three
// REJECT rows go red with `hasErrors()` false — a false ACCEPT, not a crash —
// and all four ACCEPT rows stay green. No row is vacuous.
//
// per §9.7 (v0.334), §28.11, §28.3, §4.14, §4.15

#include <string>

#include "support/aot_support.hpp"       // runAot — AOT oracle
#include "support/ownership_support.hpp" // parseAndCheckOwnership
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

// The issue repro verbatim (#5847), `using var` source, `return spans[0]`. The
// loop in Main exists so the dangling read is repeated (a one-shot UAF repro is
// vacuous); the REJECT row below never reaches it — the program does not
// compile — and the control row runs it for real.
const char* kLeakSpanSrc =
    "module M; "
    "class C "
    "  public static method LeakSpan(): Span<byte> "
    "    var spans: Array<Span<byte>> := []; "
    "    using var buf := new OwningBuffer<byte>(16); "
    "    for var j in 0..15 do buf[j] := byte(j); end for; "
    "    spans.Add(buf.AsSpan()); "
    "    return spans[0]; "
    "  end LeakSpan; "
    "  public static method Main(): int "
    "    var acc: int := 0; "
    "    var i: int := 0; "
    "    while i < 300 do "
    "      var s := C.LeakSpan(); "
    "      acc := acc + int(s[3]); "
    "      i := i + 1; "
    "    end while; "
    "    return acc; "
    "  end Main; "
    "end C;";

// The differential control: byte-identical except the source is a PARAMETER,
// which outlives this frame, so §9.7's return bullet admits it. buf[3] == 3,
// read 300 times -> 900. (900 exceeds the 8-bit exit code, so the row asserts
// on a per-call value instead — see the control row.)
const char* kParamSrc =
    "module M; "
    "class C "
    "  public static method View(buf: OwningBuffer<byte>): Span<byte> "
    "    var spans: Array<Span<byte>> := []; "
    "    for var j in 0..15 do buf[j] := byte(j); end for; "
    "    spans.Add(buf.AsSpan()); "
    "    return spans[0]; "
    "  end View; "
    "  public static method Main(): int "
    "    using var buf := new OwningBuffer<byte>(16); "
    "    for var j in 0..15 do buf[j] := byte(j); end for; "
    "    var s := C.View(buf); "
    "    return int(s[3]); "
    "  end Main; "
    "end C;";

// The §9.7 span borrow-lifetime diagnostic. Asserted as TEXT, not merely as
// "some error": the row must fail both if the compiler CRASHES on the repro and
// if it rejects for the WRONG reason (some unrelated diagnostic, or a
// §28.11 store-site rejection rather than the return-site §9.7 one).
bool rejectsHolderSpanReturn(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        // The §9.7 record borrow-lifetime rule (the span-flavoured wording).
        if (d.message.find("returned span views `buf`") == std::string::npos)
            continue;
        if (d.message.find("disposed at scope end") == std::string::npos)
            continue;
        if (d.message.find("§9.7") == std::string::npos) continue;
        if (d.message.find("§4.14") == std::string::npos &&
            d.message.find("§4.15") == std::string::npos)
            continue;
        return true;
    }
    return false;
}

}  // namespace

// ===========================================================================
// ROW 1 — REJECT. The issue's exact repro: a span stored into a borrow-bearing
// holder, then returned. The holder (`spans`) is neither a method parameter nor
// a tracked owned local, so before the fix both projection-branch checks fell
// through and the program compiled clean and read a freed buffer at runtime.
// ===========================================================================
BEZEL_TEST(collection_holder_span_escape_5847, leak_span_return_rejected) {
    auto r = parseAndCheckOwnership(kLeakSpanSrc);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsHolderSpanReturn(r));
}

// The indexer-set twin of row 1 (§28.11's OTHER borrow-store sink, named beside
// the storing methods in §28.11's element slot policy). Same holder, same
// dangling read, reached through `spans[0] :=` instead of `.Add(...)`. Pinned
// separately because it is a distinct AST shape reaching the same source set —
// a row that only ever fires for one carrier would not prove the fix.
BEZEL_TEST(collection_holder_span_escape_5847, leak_span_return_rejected_indexer_set) {
    std::string src(
        "module M; class C "
        "  public static method LeakSpan(): Span<byte> "
        "    var spans: Array<Span<byte>> := []; "
        "    using var buf := new OwningBuffer<byte>(16); "
        "    for var j in 0..15 do buf[j] := byte(j); end for; "
        "    spans[0] := buf.AsSpan(); "
        "    return spans[0]; "
        "  end LeakSpan; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    auto r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectsHolderSpanReturn(r));
}

// A span bound to a local off the SAME scope-bound source and then returned —
// the #1966 one-hop INDIRECT form, but reached through the holder rather than
// off the source directly.
//
// This is a fail-without row for #5847, not a no-regression guard: MEASURED on
// the reverted base it compiled clean, because the intermediate local `s` got
// its carried source from `spans`, and `spans` — the root the DFS then had to
// walk — carried no source set at all, so the chain terminated empty. With the
// set populated the existing #1966 machinery resolves it unchanged. Named
// separately because it proves the fix reaches the indirect sibling of row 1
// rather than only the direct projection.
BEZEL_TEST(collection_holder_span_escape_5847, holder_then_local_return_rejected) {
    std::string src(
        "module M; class C "
        "  public static method LeakSpan(): Span<byte> "
        "    var spans: Array<Span<byte>> := []; "
        "    using var buf := new OwningBuffer<byte>(16); "
        "    for var j in 0..15 do buf[j] := byte(j); end for; "
        "    spans.Add(buf.AsSpan()); "
        "    var s := spans[0]; "
        "    return s; "
        "  end LeakSpan; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    auto r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ===========================================================================
// ROW 2 — ACCEPT, the differential control. Byte-identical to the repro except
// that the source is a PARAMETER, whose storage the caller supplied and which
// outlives this frame, so §9.7's return bullet admits the shape. The fix
// records the same source set here; `isEscapeBoundLocal` is what declines it.
//
// This row is the whole point: a fix that rejected it would be a narrowing
// dressed as a fix. It runs the AOT path and asserts the VALUE (buf[3] == 3),
// not just acceptance — an admitted program that read the wrong byte would
// still be a bug. exit code 3, well inside the 8-bit range.
// ===========================================================================
BEZEL_TEST(collection_holder_span_escape_5847, param_sourced_span_return_accepted) {
    auto r = runAot(kParamSrc);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}

// The same control, REPEATED — §28.11's holder outlives the frame only while the
// caller keeps it alive, so reading the returned span 300 times in a loop is the
// same sound access as reading it once. Asserted as a compile ACCEPT (not a run)
// because a 300-iteration sum of 3 is 900, which the 8-bit exit code truncates
// to 132 (see tests/README.md, "exit codes above 255"). The value oracle for the
// control is row 2's exit code 3; this row's job is to show the loop shape is
// admitted at all, which a narrowing fix would break.
BEZEL_TEST(collection_holder_span_escape_5847, param_sourced_span_loop_accepted) {
    std::string src(
        "module M; class C "
        "  public static method View(buf: OwningBuffer<byte>): Span<byte> "
        "    var spans: Array<Span<byte>> := []; "
        "    for var j in 0..15 do buf[j] := byte(j); end for; "
        "    spans.Add(buf.AsSpan()); "
        "    return spans[0]; "
        "  end View; "
        "  public static method Main(): int "
        "    using var buf := new OwningBuffer<byte>(16); "
        "    for var j in 0..15 do buf[j] := byte(j); end for; "
        "    var acc: int := 0; "
        "    var i: int := 0; "
        "    while i < 300 do "
        "      var s := C.View(buf); "
        "      acc := acc + int(s[3]); "
        "      i := i + 1; "
        "    end while; "
        "    return acc; "
        "  end Main; "
        "end C;");
    auto r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The receiver-sourced control: the holder's span comes from `this`'s own owned
// field, whose storage outlives the frame, so §9.7's receiver-sourced form is
// admitted. Pins that the new arm does not reject the receiver-sourced shape —
// the arm sits after the `this` root's existing handling, and a source that is
// a FIELD is never escape-bound.
BEZEL_TEST(collection_holder_span_escape_5847, receiver_sourced_span_return_accepted) {
    std::string src(
        "module M; class C "
        "  public owned _buf: OwningBuffer<byte>; "
        "  public constructor() _buf := new OwningBuffer<byte>(16); end constructor; "
        "  public method View(): Span<byte> "
        "    var spans: Array<Span<byte>> := []; "
        "    for var j in 0..15 do _buf[j] := byte(j); end for; "
        "    spans.Add(_buf.AsSpan()); "
        "    return spans[0]; "
        "  end View; "
        "  public static method Main(): int "
        "    var c := new C(); "
        "    var s := c.View(); "
        "    return int(s[3]); "
        "  end Main; "
        "end C;");
    auto r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A value-typed element carries no borrow (§28.11's element slot policy table:
// a value type in a borrowing-family slot is COPIED), so storing one into a
// collection local must not manufacture a source set — and must not newly
// reject a return of that value. Pins the element-type gate in the new writer:
// without it, this row would be rejected for a "borrow" that does not exist.
BEZEL_TEST(collection_holder_span_escape_5847, value_element_holder_return_accepted) {
    std::string src(
        "module M; class C "
        "  public static method Sum(): int "
        "    var nums: Array<int> := []; "
        "    using var buf := new OwningBuffer<byte>(16); "
        "    for var j in 0..15 do buf[j] := byte(j); end for; "
        "    nums.Add(int(buf[3])); "
        "    return nums[0]; "
        "  end Sum; "
        "  public static method Main(): int return C.Sum(); end Main; "
        "end C;");
    auto r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
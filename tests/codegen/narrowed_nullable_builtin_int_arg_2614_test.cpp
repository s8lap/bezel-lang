// Codegen — issue #2614 / language spec §7.4 (Member access on nullable
// values) / stdlib §3.3 (`ReadOnlyStringSpan`) + §9.6 (`String`) + §4.14
// (`OwningBuffer<T>`, `Span<T>`, `ReadOnlySpan<T>`), v0.236.
//
// A §7.4 flow-narrowed nullable value passed as a builtin's non-nullable
// `int` parameter emitted malformed IR.
//
// §7.4: "Member access, method calls, and indexing on a nullable value are
// compile errors unless the compiler has narrowed it to non-null" — and
// "Narrowing also applies after early return". So after `if n = null then
// return …; end;` the local `n: int?` is a non-null `int` and is admissible
// as a non-nullable `int` parameter. Sema agrees: it ACCEPTS the narrowed
// form and REJECTS the un-narrowed one ("the argument to
// `ReadOnlyStringSpan.Slice` is nullable but the target type is non-nullable
// (§7)"), which is the reject-side control at the bottom of this file.
//
// Codegen did not follow. A builtin's parameter list is synthetic: each
// intrinsic lowers its own arguments ad hoc and never passes through the
// user-method argument binding that already performs the narrowed-nullable
// unwrap (LlvmBackend_Assign.cpp ~7897). So the narrowed local arrived still
// wearing its declared tagged-optional `{i8 hasVal; i32 payload}` box, and the
// site's width adjustment emitted `trunc { i8, i32 } %x to i32` (span Slice) or
// `sext { i8, i32 } %x to i64` (string Insert) — which `verifyModule` rejects:
//
//     Trunc only operates on integer / SExt only operates on integer
//     → bezel codegen: IR verification failed
//
// The fix unwraps the box to its bare payload at the affected lowering sites
// via the shared `unwrapNarrowedNullableOperand` helper
// (LlvmBackend_impl.hpp), which fires only when the lowered value carries the
// tagged-optional shape AND sema types the argument as a nullable PRIMITIVE.
//
// These tests use the AOT oracle (runAot / runAotOut): the defect is a
// verifyModule failure, and verifyModule runs on the AOT path. Each test
// asserts the OBSERVED RESULT of the call — the sliced text, the inserted
// string, the element actually addressed — not merely that the program
// compiled, so that a fix which unwrapped the WRONG field (the `hasVal` flag
// instead of the payload) would still fail them.
//
// Demonstrated mechanically: every narrowed case below FAILS on the unpatched
// compiler (ran=false, "IR verification failed") and PASSES with the fix; the
// two plain-argument positive controls and the un-narrowed reject control pass
// either way, which is what makes them controls.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotOut — AOT compile path
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// runAot reports a toolchain SKIP as ran=false with a diagnostic containing
// "SKIP"; treat that as a no-op rather than a failure.
bool aotWasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string_view::npos) return true;
    return false;
}

// §7.4 narrowing harness: `n` comes back from a flag-decided helper as `int?`,
// then an early return on null narrows it to a non-null `int` for `body`.
// `body` is spliced in after the narrowing check and must `return` an int.
std::string narrowed(const std::string& body) {
    return "module M; import System.Collections.Generic; "
           "class C "
           "  private static method N(flag: bool, k: int): int? "
           "    if flag then return k; end; "
           "    return null; "
           "  end N; "
           "  public static method Main(): int "
           + body +
           "  end Main; "
           "end C;";
}

// Per tests/README.md: runAot's WIFSIGNALED path synthesizes an Error
// diagnostic for a runtime crash too, so a genuine compile-time rejection must
// be distinguished by its message TEXT, never by `!r.ran` alone.
bool rejectsMentioning(const std::string& program, const char* keyword) {
    JitResult r = runAot(program);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(keyword) != std::string_view::npos) return true;
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// ReadOnlyStringSpan.Slice — the issue's repro A (stdlib §3.3:
// `public pure method Slice(start: int, length: int): ReadOnlyStringSpan`).
// ---------------------------------------------------------------------------

// #2614: narrowed `int?` at the START position. "hello".AsSpan().Slice(1, 2)
// is "el" — asserting the TEXT proves the payload (1), not the hasVal flag
// (also 1 here by luck) nor 0, reached the start operand.
BEZEL_TEST(codegen_narrowed_builtin_int_2614, ross_slice_narrowed_start) {
    JitResult r = runAot(narrowed(
        "    var sp := \"hello\".AsSpan(); "
        "    var n := N(true, 1); "                    // n: int?, runtime 1
        "    if n = null then return 0; end; "         // §7.4 narrowing
        "    var s2 := sp.Slice(n, 2); "               // narrowed int? as start
        "    if s2.ToString() = \"el\" then return 7; end; "
        "    return 1; "));
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// #2614: narrowed `int?` at the LENGTH position — the second argument is
// lowered by a separate statement at the same site, so it needs its own
// unwrap. "hello".AsSpan().Slice(1, 3) is "ell".
BEZEL_TEST(codegen_narrowed_builtin_int_2614, ross_slice_narrowed_length) {
    JitResult r = runAot(narrowed(
        "    var sp := \"hello\".AsSpan(); "
        "    var n := N(true, 3); "                    // n: int?, runtime 3
        "    if n = null then return 0; end; "         // §7.4 narrowing
        "    var s2 := sp.Slice(1, n); "               // narrowed int? as length
        "    if s2.ToString() = \"ell\" then return 7; end; "
        "    return 1; "));
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// #2614: the v0.195 ONE-ARGUMENT `Slice(start)` overload (stdlib §3.3:
// "from offset to end"). It shares the site but takes a different branch —
// the length is computed, not lowered — so the start unwrap must still fire.
// "hello".AsSpan().Slice(1) is "ello".
BEZEL_TEST(codegen_narrowed_builtin_int_2614, ross_slice_one_arg_narrowed) {
    JitResult r = runAot(narrowed(
        "    var sp := \"hello\".AsSpan(); "
        "    var n := N(true, 1); "
        "    if n = null then return 0; end; "         // §7.4 narrowing
        "    var s2 := sp.Slice(n); "
        "    if s2.ToString() = \"ello\" then return 7; end; "
        "    return 1; "));
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ---------------------------------------------------------------------------
// string int-parameter family — the issue's repro B. These lower through the
// SHARED `getIntArg` helper (LlvmBackend_String.cpp), so the single unwrap
// there covers the family; the two cases below pin two distinct members of it
// to prove the fix is the family-wide one and not an Insert special case.
// ---------------------------------------------------------------------------

// #2614: string.Insert (stdlib §9.6:
// `public pure method Insert(startIndex: int, value: string): string`) with a
// narrowed `int?` index. "abc".Insert(1, "X") is "aXbc" — the emitted `sext`
// was the reported malformed instruction.
BEZEL_TEST(codegen_narrowed_builtin_int_2614, string_insert_narrowed_index) {
    BEZEL_EXPECT_EQ(runAotOut(narrowed(
        "    var s := \"abc\"; "
        "    var n := N(true, 1); "                    // n: int?, runtime 1
        "    if n = null then return 0; end; "         // §7.4 narrowing
        "    Console.WriteLine(s.Insert(n, \"X\")); "
        "    return 0; ")), "aXbc");
}

// #2614: string.Remove (stdlib §9.6:
// `public pure method Remove(startIndex: int): string`) with a narrowed
// `int?`. "abcde".Remove(2) is "ab". A DIFFERENT method through the same
// getIntArg helper — if the fix had been applied at Insert's own call site
// instead of the helper, this case would still fail.
BEZEL_TEST(codegen_narrowed_builtin_int_2614, string_remove_narrowed_start) {
    BEZEL_EXPECT_EQ(runAotOut(narrowed(
        "    var s := \"abcde\"; "
        "    var n := N(true, 2); "                    // n: int?, runtime 2
        "    if n = null then return 0; end; "         // §7.4 narrowing
        "    Console.WriteLine(s.Remove(n)); "
        "    return 0; ")), "ab");
}

// ---------------------------------------------------------------------------
// Span<T>.Slice — the sibling span lowering (stdlib §4.14), a second site with
// the identical shape a few hundred lines from the ReadOnlyStringSpan one.
// ---------------------------------------------------------------------------

// #2614: Span<int>.Slice with a narrowed `int?` start. The buffer holds
// 10,11,12,13,14; Slice(2, 2) starts at element 12, so returning s2[0] proves
// the start operand carried the payload 2 and not the hasVal flag 1 (which
// would yield 11) or 0 (which would yield 10).
BEZEL_TEST(codegen_narrowed_builtin_int_2614, span_slice_narrowed_start) {
    JitResult r = runAot(narrowed(
        "    using var ob := new OwningBuffer<int>(5); "
        "    var sp := ob.AsSpan(); "
        "    sp[0] := 10; sp[1] := 11; sp[2] := 12; sp[3] := 13; sp[4] := 14; "
        "    var n := N(true, 2); "                    // n: int?, runtime 2
        "    if n = null then return 0; end; "         // §7.4 narrowing
        "    var s2 := sp.Slice(n, 2); "
        "    return s2[0]; "));
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 12);
}

// ---------------------------------------------------------------------------
// CONTROLS — these pass with and without the fix. They exist to prove the fix
// neither changes the plain-argument lowering nor widens what sema accepts.
// ---------------------------------------------------------------------------

// #2614 POSITIVE CONTROL: plain (non-nullable) `int` arguments must lower
// exactly as before. unwrapNarrowedNullableOperand is shape-guarded, so a bare
// i32 is returned untouched. This is the test that fails if the unwrap were
// over-eager.
BEZEL_TEST(codegen_narrowed_builtin_int_2614, plain_int_args_unchanged) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class C public static method Main(): int "
        "  var sp := \"hello\".AsSpan(); "
        "  Console.WriteLine(sp.Slice(1, 2).ToString()); "
        "  Console.WriteLine(\"abc\".Insert(1, \"X\")); "
        "  return 0; "
        "end Main; end C;"), "el\naXbc");
}

// #2614 REJECT CONTROL: an UN-narrowed `int?` must STILL be a sema rejection.
// This is the soundness hinge of the whole fix: because sema rejects here, the
// codegen unwrap can only ever see a value §7.4 already proved non-null, so
// extracting the payload unconditionally is safe. If this test ever starts
// passing the argument through, the unwrap would silently read the payload of
// a null optional and #2614's compile-time ICE would become a silent wrong
// value — strictly worse. Asserted on diagnostic TEXT per tests/README.md.
BEZEL_TEST(codegen_narrowed_builtin_int_2614, unnarrowed_nullable_still_rejected) {
    BEZEL_EXPECT_TRUE(rejectsMentioning(
        "module M; "
        "class C public static method Main(): int "
        "  var sp := \"hello\".AsSpan(); "
        "  var n: int? := null; "
        "  var s2 := sp.Slice(n, 1); "     // NOT narrowed — must reject
        "  return s2.Length; "
        "end Main; end C;", "nullable"));
}

// #2614 REJECT CONTROL — `Span<T>`/`ArraySpan<T>.Slice` (stdlib §4.14/§4.15).
// This site was NOT gated: checkBuiltinSpanCallArgs early-returned unless the
// receiver was ReadOnlyStringSpan, and checkBuiltinCollectionCallArgs'
// isItemPosition explicitly EXCLUDES range params. The first version of this
// fix applied the codegen unwrap here anyway, so `sp.Slice(null, 2)` stopped
// being a verifyModule ICE and instead compiled, read the null box's zero
// payload as the start, and RETURNED ELEMENT 0 — a silent wrong value. The fix
// extends the sema gate to the span-buffer families so the unwrap's
// precondition holds by construction.
BEZEL_TEST(codegen_narrowed_builtin_int_2614, span_slice_unnarrowed_rejected) {
    BEZEL_EXPECT_TRUE(rejectsMentioning(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  var n: int? := null; "
        "  using var ob := new OwningBuffer<int>(5); "
        "  var sp := ob.AsSpan(); "
        "  var s2 := sp.Slice(n, 2); "     // NOT narrowed — must reject
        "  return s2[0]; "
        "end Main; end C;", "nullable"));
}

// #2614 REJECT CONTROL — `string.AsSpan(start[, length])` (stdlib §9.6).
// AsSpan lowers its int arguments through the same `getIntArg` helper the fix
// patched, but was absent from checkBuiltinStringCallArgs' `intPos` set, so
// sema type-checked its arguments not at all. Same failure mode as above:
// `"hello".AsSpan(null)` compiled and silently behaved as `AsSpan(0)`,
// returning Length 5. Now gated.
BEZEL_TEST(codegen_narrowed_builtin_int_2614, string_asspan_unnarrowed_rejected) {
    BEZEL_EXPECT_TRUE(rejectsMentioning(
        "module M; "
        "class C public static method Main(): int "
        "  var n: int? := null; "
        "  var sp := \"hello\".AsSpan(n); "   // NOT narrowed — must reject
        "  return sp.Length; "
        "end Main; end C;", "nullable"));
}

// #2614 NARROWED POSITIVE CONTROL for the two newly-gated sites: extending a
// sema gate risks FALSE REJECTS, which are worse than the bug. The §7.4
// narrowed form must still compile and produce the right value at both.
// "hello".AsSpan(1).ToString() is "ello"; the span slice at start 2 of
// 10,11,12,13,14 is element 12.
BEZEL_TEST(codegen_narrowed_builtin_int_2614, newly_gated_sites_accept_narrowed) {
    JitResult r = runAot(narrowed(
        "    using var ob := new OwningBuffer<int>(5); "
        "    var sp := ob.AsSpan(); "
        "    sp[0] := 10; sp[1] := 11; sp[2] := 12; sp[3] := 13; sp[4] := 14; "
        "    var n := N(true, 2); "
        "    if n = null then return 0; end; "         // §7.4 narrowing
        "    var s2 := sp.Slice(n, 2); "               // newly gated, narrowed
        "    var k := N(true, 1); "
        "    if k = null then return 0; end; "         // §7.4 narrowing
        "    var ss := \"hello\".AsSpan(k); "          // newly gated, narrowed
        "    if ss.ToString() <> \"ello\" then return 1; end; "
        "    return s2[0]; "));
    if (aotWasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 12);
}

// #2614 FALSE-REJECT CONTROL: the `Range<int>` overloads must be untouched.
// `AsSpan(r: Range<int>)` and the span families' non-int positions pass a Named
// (non-primitive) argument; the gate's Primitive guard must skip them rather
// than demand an `int`. "hello".AsSpan(1..<4).ToString() is "ell".
BEZEL_TEST(codegen_narrowed_builtin_int_2614, range_overload_not_false_rejected) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "class C public static method Main(): int "
        "  Console.WriteLine(\"hello\".AsSpan(1..<4).ToString()); "
        "  return 0; "
        "end Main; end C;"), "ell");
}

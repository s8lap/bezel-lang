// Spec-derived tests for issue #2268 — §9.7 / §22.5 (v0.236): the §9.7
// borrow-bearing-record RETURN walker must key its borrow-carrying-field
// check off the BOUND positional field (per sema's §22.5
// bindArgsToParams), not off the argument's source position.
//
// The fix (Ownership_Checks.cpp:3252-3330, checkRecordBorrowEscapesOnReturn):
// the pre-fix loop iterated `ne->args` by source position and looked up
// `recordDecl->positionalFields[i]`, so a `new Holder(N := s.Count, S :=
// p.AsSpan())` call (args[0]=`N:=`, args[1]=`S:=`) checked args[0] (the
// `int` value `s.Count`) against the field at index 0 (`S: span`,
// borrow-carrying) — the wrong direction. `s.Count`'s root `s` was then
// wrongly reported as the borrow's escape-bound source (false-reject). The
// inverse — a named-arg reorder where the borrow-carrying arg sat at
// source index 0 of a value-typed field — slipped past the walker
// (under-reject → use-after-free).
//
// Each test below pins one of these directions; the ACCEPT side runs the
// full AOT oracle so a silently wrong value is caught too, and the REJECT
// side asserts on diagnostic TEXT (the §9.7 message) rather than the
// coarse `hasErrors()` (runAot synthesizes a crash-error on SIGSEGV too,
// per the lane-protocol §3).
//
// per §9.7 (v0.236), §22.5 (v0.236), #2268.

#include <string>

#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// True iff an ERROR diagnostic whose message contains `needle` was emitted.
// Mirrors record_direct_new_non_borrow_arg_2150_test.cpp's hasError pattern.
bool hasError(const std::string& src, const std::string& needle) {
    auto r = parseAndCheckOwnership(src);
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

// ===========================================================================
// DIRECTION 1 — FALSE REJECT (#2150 fixed, #4428 closes the parameter arm).
//
// §9.7 / §22.5: a record `Holder(S: ArraySpan<int>, N: int)` declared with
// the span field FIRST. A return of `new Holder(N := s.Count, S :=
// p.AsSpan())` with `s` a scope-bound owned local (using var) — the post-
// #2150 walker iterates the BOUND field and skips the int field's `s.Count`
// (a value, no borrow) so it no longer falsely rejects on `s`. The span
// field's arg (`p.AsSpan()`, rooted at `p`, a parameter) is the BORROW source,
// and §9.7 v0.301 closes the parameter arm — "a record type announces no
// source set at all, so a caller has nothing but the receiver to re-establish
// and a parameter-sourced record return would hand back a borrow no call
// site could bound" — so the program is REJECTED on the `S` field's arg.
// Renamed and re-polarized below; the pre-#2150 false-reject on `s` (a
// pre-existing bug pinned by the test name) is no longer reproducible either
// way, because the test's parameter arm was the original false-accept.
// // per §9.7, §22.5, v0.301.
// ===========================================================================
BEZEL_TEST(named_arg_walker_2268, span_field_param_rooted_rejected_9_7_v301) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "record Holder(S: ArraySpan<int>, N: int); "
        "class C "
        "  public static method View(p: Array<int>): Holder "
        "    var s := new Array<int>(); s.Add(11); "
        "    return new Holder(N := s.Count, S := p.AsSpan()); "
        "  end View; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::hasErrorMessage(
        r, "returned borrow-bearing record carries a borrow of parameter `p`"));
}

// ===========================================================================
// DIRECTION 2 — UNDER REJECT (under-rejection, now DETECTED).
//
// §9.7 / §22.5: a record `Holder(S: ArraySpan<int>, N: int)` declared with
// the span field FIRST. A return of `new Holder(N := 0, S := s.AsSpan())`
// with `s` a scope-bound owned local must BE rejected: the span field's
// `s.AsSpan()` borrows `s`, a local disposed at scope end — the returned
// Holder carries a borrow that outlives its source (UAF).
//
// PRE-FIX: walker iterated args by source index and matched args[0]
// (`N := 0`, an int literal) against field[0] (`S: span`,
// borrow-carrying); `identifierRoot(0)` is the literal, with no
// escape-bound root → no rejection. The runtime read of `h.S[0]` after
// `s`'s auto-drop is a classic §9.7 use-after-free (confirmed: pre-fix the
// program compiles rc=0 and SEGV's under ASan).
//
// Assert on diagnostic TEXT (§9.7), not the coarse `rejects()` —
// runAot's `WIFSIGNALED` path synthesizes a generic Error diagnostic on
// a SIGSEGV, so a coarse accept-everything check would mask the real
// under-reject path.
// ===========================================================================
BEZEL_TEST(named_arg_walker_2268, under_reject_span_field_binds_escape_bound_rejected) {
    std::string src =
        "module M; "
        "record Holder(S: ArraySpan<int>, N: int); "
        "class C "
        "  public static method Make(): Holder "
        "    using var s := new Array<int>(); s.Add(11); "
        "    return new Holder(N := 0, S := s.AsSpan()); "
        "  end Make; "
        "  public static method Main(): int return 0; "
        "end C;";
    // Parse-and-check: the sema+ownership pass must report the §9.7 escape.
    // (runAotChecked would also see a UAF at runtime, but the static reject
    // is the load-bearing signal — the runtime crash is the consequence the
    // fix prevents.)
    BEZEL_EXPECT_TRUE(hasError(src, "§9.7"));
    BEZEL_EXPECT_TRUE(hasError(src, "borrow-bearing record"));
    BEZEL_EXPECT_TRUE(hasError(src, "carries a borrow of"));
}

// ===========================================================================
// CONTROL — declaration-order named args: a `new Holder(S := p.AsSpan(),
// N := s.Count)` call where the NAMED args are in declaration order. The
// walker iterates the BOUND field, so the span field gets `p.AsSpan()`
// (borrow from param p) and the int field gets `s.Count` (value, skipped).
// §9.7 v0.301 closes the parameter arm: this is REJECTED at the return
// site on the `S` field's argument (`p.AsSpan()`, rooted at the parameter
// `p`). Re-polarized from the pre-#4428 ACCEPT text; the named-arg lookup
// is still load-bearing — see DIRECTION 2 (named-arg, borrow-first), which
// gates the SAME logic in reverse field order.
// ===========================================================================
BEZEL_TEST(named_arg_walker_2268, declaration_order_named_args_rejected_9_7_v301) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "record Holder(S: ArraySpan<int>, N: int); "
        "class C "
        "  public static method View(p: Array<int>): Holder "
        "    var s := new Array<int>(); s.Add(11); "
        "    return new Holder(S := p.AsSpan(), N := s.Count); "
        "  end View; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::hasErrorMessage(
        r, "returned borrow-bearing record carries a borrow of parameter `p`"));
}

// ===========================================================================
// CONTROL — positional args unchanged (§9.7 precision #2150 covered this
// shape; the named-arg fix must not regress the positional leg). The
// §9.7 v0.301 ruling closes the parameter arm: this is REJECTED at the
// return site on the `S` field's argument (`p.AsSpan()`, rooted at the
// parameter `p`).
// ===========================================================================
BEZEL_TEST(named_arg_walker_2268, positional_args_rejected_9_7_v301) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "record Holder(S: ArraySpan<int>, N: int); "
        "class C "
        "  public static method View(p: Array<int>): Holder "
        "    var s := new Array<int>(); s.Add(11); "
        "    return new Holder(p.AsSpan(), s.Count); "
        "  end View; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(bezel::test::hasErrorMessage(
        r, "returned borrow-bearing record carries a borrow of parameter `p`"));
}

} // namespace

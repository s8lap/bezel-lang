// Codegen — fix #3890 (stdlib §4.8/§4.9, spec §27.5/§28.9, v0.294):
// `Dictionary<K,V>.GetOrAdd(key, factory)` / `OwningDictionary<K,V>.GetOrAdd`
// whose factory is an inline lambda LITERAL that captures a string local
// frees that local's shared box CONTENT on closure release (at the call's
// join point), while the outer local is still live — a heap-use-after-free
// on any later read.
//
// PROVENANCE. Filed by the reviewer of fix #3886 via ASan fault injection,
// and bisected to before both fix #3871 (this file's own capturing-factory
// leak fix, which admits the factory's captures into `cellFreeEligible_`)
// and fix #3886 (the Dictionary/OwningDictionary string-KEY release
// relaxation) existed — neither introduced nor worsened it. Neither of
// their own regression tests covers this combination: #3886's GetOrAdd row
// deliberately uses a NON-CAPTURING factory
// (`dict_call_arg_key_local_release_3886_test.cpp`), and #3871's rows
// capture an `int` (`captured`), never the KEY string itself
// (`dict_get_or_add_capturing_factory_leak_3871_test.cpp`). The untested
// shape is exactly this issue's: a factory literal capturing the SAME
// string local that is also the key argument, read again afterwards.
//
// ROOT CAUSE, traced (not guessed) by reading the three consumers of
// `lowerLambda`'s per-capture `CaptureEntry` (LlvmBackend_Exprs2.cpp):
//
//   Fix #3871 admits the GetOrAdd factory's captures into
//   `cellFreeEligible_` (`collectCellFreeEligible`, LlvmBackend_Setup.cpp) —
//   correctly: the factory closure never outlives the call (this arm
//   unconditionally `emitClosureRelease`s it at `goa.end` on BOTH the hit
//   and miss paths, per stdlib §4.8's "GetOrAdd's factory is call-bounded").
//   But `cellFreeEligible_` membership alone only controls whether the
//   CELL (the 8-byte box holder) is reclaimed at scope exit — it says
//   nothing about WHEN the captured string/closure CONTENT held in that
//   cell is released. That is a separate question, answered by
//   `lowerLambda`'s "first capture's envdtor is the deferred scope-end
//   CONTENT release" model (fix #672/#802): every capturing env's envdtor
//   unconditionally releases the string/closure content UNLESS
//   `ce.suppressContentRcInEnvdtor` defers that release to the captured
//   local's own OWNER scope instead — sound only when the envdtor runs at
//   or after the local's natural scope end.
//
//   Fix #3752 and fix #3778 already prove that widening is needed whenever
//   the CAPTURING closure's env dies BEFORE the captured local's own scope
//   ends: #3752 covers a capture in a strictly DEEPER dispose scope
//   (`declaredShallower`); #3778 covers an ordinary call-ARGUMENT lambda
//   literal, whose closure is a temporary released at end of statement
//   (`inArgumentLambda_`, set by `buildOrderedArgs` in
//   LlvmBackend_Assign.cpp around `lowerExpr` for a lambda-literal
//   argument). GetOrAdd's factory is EXACTLY the #3778 shape — a call-
//   argument lambda literal whose closure is released at end of the
//   GetOrAdd call, before the captured local's (same-scope) natural scope
//   end — but GetOrAdd is a BUILTIN, lowered directly in
//   `LlvmBackend_Calls.cpp`'s `m == "GetOrAdd"` arm rather than through
//   `buildOrderedArgs`, so `inArgumentLambda_` was never set for it. With
//   neither `declaredShallower` (same scope: false) nor `inArgumentLambda_`
//   (never armed: false) true, `ce.suppressContentRcInEnvdtor` never fires,
//   and the envdtor's unconditional content release runs at `goa.end` —
//   before `var n := key.Length;`'s read of the still-referenced local.
//
// THE FIX (LlvmBackend_Calls.cpp, the `GetOrAdd` arm): set
// `inArgumentLambda_ = true` around `lowerExpr(*call.args[1].value)` when
// that argument is a lambda LITERAL — mirroring #3778's own site exactly,
// reusing the SAME flag and the SAME consuming gate in
// `LlvmBackend_Exprs2.cpp` rather than inventing a second one. This defers
// the content release to the captured local's OWNER scope (its own natural
// scope end) instead of the envdtor, which is exactly where the release
// belongs once the closure that would otherwise perform it dies early.
// Narrowly gated on the argument actually being `ast::LambdaExpr` (a
// var/field-bound factory is `ast::IdentifierExpr`, a sibling under
// `ast::Expr` that can never satisfy the `dynamic_cast` — that shape's own
// separate defect, an unrelated whole-closure leak, is #3885).
//
// CHOICE MADE: leak vs. double-free vs. UAF. This fix does not suppress
// the closure's release (which would either leak the content, if declined
// unconditionally, or double-free it against the SAME reference the owner
// scope also releases if suppressed carelessly) — it retargets WHICH scope
// performs the SAME single release, exactly reusing #3752/#3778's own
// "exactly once, correctly timed" accounting: the local's one +1 (its
// own scope-end release already suppressed by escape detection, in favor
// of the deferred registration this fix arms) is released exactly once,
// at the scope where the local actually dies. Neither a leak nor a
// double-free is introduced; the UAF is closed by moving the SAME release
// to the correct point in time.
//
// Every row here is `runAotChecked` (ASan+LSan on the GENERATED program) and
// loop-amplified 300x (LANE_RULES 200-500x band): a one-shot run can pass
// on both sides of a leak-shaped fix because a single allocation is still
// stack-reachable at process exit, and — for the UAF direction specifically
// — ASan's own fault-on-first-touch behavior means even ONE iteration
// already suffices for the crash; the loop instead pins that the fix does
// not trade the UAF for a leak (each iteration's string must actually be
// freed, or LSan's 300x-amplified report would catch it) and stays clean at
// scale, not just once.

#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::checkedToolchainSkipped;
using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

std::string prog(const std::string& mainBody) {
    return
        "module M; "
        "class C public static method Main(): int " + mainBody +
        " end Main; end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// stdlib §4.8 / spec §27.5 / §28.9 — MISS path, the issue's own repro shape
// verbatim: a fresh `using var` Dictionary each iteration (the literal key
// is always absent, so the factory runs and captures `key` — the SAME
// local that is also the KEY argument), then `key` is read again AFTER the
// call. Keys are all 4 characters ("k" + 3 digits, i in [100,400)) so the
// expected length is a FIXED, independently-known constant (4), not merely
// "whatever the two reads happen to agree on".
//
// FAIL-WITHOUT the fix (measured on the unmodified base, `bezel_tests_
// codegen` relinked): AddressSanitizer heap-use-after-free, "READ of size 8
// ... freed by thread T0 here: ... in bz$lam$0$envdtor", on the FIRST
// iteration (an ASan fault is immediate — it does not need 300 iterations to
// manifest, unlike a leak). PASS-WITH: clean, exit 0.
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_get_or_add_capturing_factory_string_key_uaf_3890,
           miss_path_capturing_factory_reads_key_after_call_27_5) {
    JitResult r = runAotChecked(prog(
        "var i := 100; "
        "while i < 400 do "
        "  using var d := new Dictionary<string, int>(); "
        "  var key := $\"k{i}\"; "
        "  var v := d.GetOrAdd(key, lambda(x: string) => key.Length); "
        "  var n := key.Length; "
        "  if n <> 4 or v <> 4 then return 1; end; "
        "  i := i + 1; "
        "end; "
        "return 0; "));
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// stdlib §4.8 / spec §27.5 / §28.9 — HIT path: the key is pre-populated (with
// a sentinel value -1, distinguishable from `key.Length` == 4), so the
// factory is NEVER INVOKED — but the closure (and its capture of `key`) is
// still CONSTRUCTED and released at `goa.end` regardless of hit/miss (the
// hit/miss branch only gates whether the factory BODY runs, per GetOrAdd's
// own codegen comment), so the hazard is identical on this path. `key` is
// read again after the call.
//
// FAIL-WITHOUT the fix: identical heap-use-after-free, same envdtor
// signature, on the FIRST iteration. PASS-WITH: clean, exit 0, and `v == -1`
// confirms the factory genuinely did not run (a change that accidentally
// made GetOrAdd re-invoke the factory on a hit would also be caught here).
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_get_or_add_capturing_factory_string_key_uaf_3890,
           hit_path_capturing_factory_reads_key_after_call_27_5) {
    JitResult r = runAotChecked(prog(
        "var i := 100; "
        "while i < 400 do "
        "  using var d := new Dictionary<string, int>(); "
        "  var key := $\"k{i}\"; "
        "  d.Add(key, -1); "
        "  var v := d.GetOrAdd(key, lambda(x: string) => key.Length); "
        "  var n := key.Length; "
        "  if n <> 4 or v <> -1 then return 1; end; "
        "  i := i + 1; "
        "end; "
        "return 0; "));
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// CONTROL — same capturing-factory shape, but `key` is NEVER read again
// after the GetOrAdd call (only `v` is checked). This is the shape #3871's
// own tests exercise (a captured local read via `v.Tag`, never the captured
// local a second time through its OWN name after the call) and it PASSES
// both WITHOUT and WITH this fix: with the local's own scope-end release
// already suppressed in favor of the (early, in the pre-fix tree) envdtor
// release, nothing observes the freed content and nothing double-frees it
// either. Included to isolate that the bug is specifically about the
// POST-CALL READ hazard, not about capturing a string per se — this row is
// NOT expected to distinguish the fix (recorded, not a guard-sensitivity
// claim for this row).
// ---------------------------------------------------------------------------
BEZEL_TEST(dict_get_or_add_capturing_factory_string_key_uaf_3890,
           control_capturing_factory_no_read_after_call_27_5) {
    JitResult r = runAotChecked(prog(
        "var i := 100; "
        "while i < 400 do "
        "  using var d := new Dictionary<string, int>(); "
        "  var key := $\"k{i}\"; "
        "  var v := d.GetOrAdd(key, lambda(x: string) => key.Length); "
        "  if v <> 4 then return 1; end; "
        "  i := i + 1; "
        "end; "
        "return 0; "));
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

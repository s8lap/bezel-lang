// Codegen — issue #3427 hole 3 (§21.1 runtime `const` locals, §28.9 / §28.11
// "reclaimed with the instance", spec v0.286): `Emitter::collectSafeViewAliases`
// scanned a body's statements with `dynamic_cast<const ast::LocalDecl*>(s)`, so
// a §21.1 runtime `const` binding a pure-`IEnumerable<T>` VIEW of a borrowing
// array/set/dict local was never recognized as a SAFE (non-escaping) alias of
// that array. `StringEscapeWalker::stmt()`'s own `ast::LocalConstDecl` arm
// likewise never consulted `safeViewAliases` (only its `ast::LocalDecl`
// sibling arm did), so — independent of the collector's own scan — a `const`
// view binding fell through to that arm's unconditional
// `expr(lc->value.get(), /*safeRead=*/false)`, tainting the array as escaped.
// Both gaps had to be closed together: widening only the collector's scan
// (`views`/`out`) would have populated `safeViewAliases` with the `const`
// view's node pointer, but nothing would ever have looked it up, because the
// ONLY reader of that set (`StringEscapeWalker::stmt()`'s `LocalDecl` arm) is
// keyed on the dynamic type of the statement BEING WALKED, not of the array
// being tested.
//
// FIX (two hunks, both in LlvmBackend_Setup.cpp):
//   1. `collectSafeViewAliases`'s `views` vector is widened from
//      `const ast::LocalDecl*` to `const ast::Node*` and its scan reads the
//      view's initializer/type through `Emitter::localDeclInitView` — the
//      single authority #3402 hoisted, and the same precedent
//      `collectBorrowingArrayFreeEligible` already applies (its own header
//      comment at this file's fix-issue-2802 hunk is the documented
//      precedent for exactly this widening). The `views` consumption loop is
//      re-typed to match.
//   2. `StringEscapeWalker::stmt()`'s `ast::LocalConstDecl` arm gains the same
//      `borrowingArrayMode && safeViewAliases->count(...) &&
//      isTargetIdent(...)` early-return its `ast::LocalDecl` sibling arm has
//      had since #1871 — mirrored byte-for-byte.
//
// §6b EXCLUSION DIFF. The pre-fix collector scan excluded every §21.1 `const`
// view binding from `safeViewAliases` unconditionally; the pre-fix walker arm
// (independently) never admitted ANY const view as safe regardless of what the
// collector produced. `localDeclInitView` recognizes only `ast::LocalDecl` /
// `ast::LocalConstDecl` (all-null view otherwise, matching the old
// dynamic_cast's null answer for every other node), so the widened collector
// admits EXACTLY the §21.1 const-view case beyond the unchanged `var` case; the
// widened walker arm requires the SAME `isTargetIdent`/`borrowingArrayMode`
// conjunction the `var` arm requires, so it admits nothing beyond the
// symmetric const case either.
//
// ⚠⚠ RESIDUAL — THIS FIX IS PARTIAL, NOT A FULL CLOSE. Re-deriving the issue's
// own repro on this lane's HEAD via `scripts/agent_check.sh --probe --asan`
// (200 iterations, matching the issue thread's own measurement) found the
// pre-fix leak split into TWO independent causes that happened to be measured
// together as one number:
//
//   (a) the ARRAY's own backing (`bz_array_alloc`) — closed by this fix.
//   (b) the CONST VIEW's own wrapper frame (`wrapArrayAsIEnumerable`'s
//       `{vtable, arr, idx, esz}` struct) — NOT closed here. Its scope-end
//       disposal is `Emitter::registerIterableSourceDisposal`
//       (LlvmBackend_Setup.cpp:14326), a **150+ line function** whose single
//       caller is `lowerStmt`'s `ast::LocalDecl` arm
//       (LlvmBackend_Stmts.cpp:169) — `lowerStmt`'s `ast::LocalConstDecl` arm
//       (LlvmBackend_Stmts.cpp:293-301) never calls it at all, for any const,
//       regardless of what the collector above admits. Fixing (b) needs both
//       `registerIterableSourceDisposal`'s body (this file — signature +
//       every internal `ld->`-field read widened to `localDeclInitView`) AND
//       the missing call wired into LlvmBackend_Stmts.cpp — a file this run's
//       ownership partition assigns to a different lane. Filed as issue #3640
//       with the isolated measurement and the two-file fix shape.
//
// Measured via `scripts/agent_check.sh --probe --asan`, 200 iterations of the
// issue's own repro (a fresh `Array<int>` literal + a `const`/`var`
// `IEnumerable<int>` view, consumed by a `for`):
//
//   spelling   pre-fix (main)         after THIS fix          var control
//   const      30400 B / 600 allocs   6368 B / 199 allocs *   —
//   var        clean                  —                      clean (both)
//
// (* the residual 6368 B / 199 ≈ 32.00 B/alloc uniformly — consistent with
// exactly one un-freed wrapper frame per iteration, issue #3640.)
//
// Because the `const` spelling still leaks after this fix, a bare
// `runAotChecked` PASS/FAIL (or an `r.ran`/`exitCode` check) reports the same
// red result before and after this hunk — NOT a FAIL-WITHOUT/PASS-WITH
// oracle. ⚠ An earlier revision of this test used `runAotCheckedLenient`
// (LSan off) and asserted only the summed VALUE; per-fix review compiled the
// PRE-fix compiler under the identical lenient ASAN_OPTIONS and got the
// SAME "60300, exit=0" on both sides — the row could not fail whether the
// fix was present or reverted (the vacuous-row pattern). The
// choice_string_field_call_release_2428_test.cpp precedent that revision
// cited is a weaker analogy than it looked: that row covers a PERMANENT,
// by-design conservative exclusion never expected to close; this residual
// (issue #3640) is a LIVE one, expected to close.
//
// FIX: `JitResult.diagnostics` retains the sanitizer report text — including
// the `SUMMARY: AddressSanitizer: NNNN byte(s) leaked` line — even when
// strict `runAotChecked` reports `r.ran == false` (aot_support.hpp populates
// both `stdoutText` and the diagnostic's sanitizer-report text BEFORE
// checking whether the sanitizer fired). The `const` test below runs under
// the STRICT oracle, parses that byte count out of the diagnostic text via
// `summaryLeakedBytes` (below), and asserts it below a threshold that sits
// comfortably between the pre-fix combined leak (~30400 B) and the post-fix
// wrapper-only residual (~6368 B) — a real FAIL-WITHOUT/PASS-WITH assertion,
// mechanically reverted and re-measured to prove both directions (see the
// commit body), that will also go red if issue #3640's eventual fix
// regresses part (a) back open. `stdoutText` is asserted too (still
// captured on the sanitizer-fired path), so a wrong VALUE or an early crash
// that truncates the loop is caught independently of the byte count.
//
// The `var` twin has ALWAYS been fully clean (no wrapper-frame gap: `var`'s
// `LocalDecl` arm has always called `registerIterableSourceDisposal`), so it
// is asserted with the strict `runAotChecked` oracle as a real regression
// guard.
//
// per §21.1, §28.9, §28.11 (issue #3427; residual filed as issue #3640)

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT+ASan
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
// ⚠ AOT exit status truncates to 8 bits (LANE_RULES §3) — the summed value
// here (60300) exceeds 255, so it is read back via `Console.WriteLine` /
// `stdoutText`, NOT `exitCode` (unlike
// array_ienumerable_view_backing_reclaim_1871_test.cpp's own helper, whose
// totals all stay under 256 and so can use the exit code directly).
// A leak / double-free / use-after-free flips r.ran to false -> "<did-not-run>".
std::string run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}

// The issue's own repro, parameterized on the view local's keyword.
std::string program(const char* viewKeyword) {
    return std::string(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      var arr: Array<int> := [i, i + 1, i + 2]; "
        "      ") + viewKeyword +
        " v: IEnumerable<int> := arr; "
        "      for var x in v do "
        "        acc := acc + x; "
        "      end for; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;";
}
}  // namespace

// ── PARITY CONTROL (strict oracle — this spelling has always been fully
// clean): the `var` twin. sum_{i=0}^{199} (3i+3) = 60300. `registerIterableSourceDisposal`
// has always run for `var`, so both the array backing AND the wrapper frame
// are reclaimed. // per §28.9, §28.11 (issue #3427)
BEZEL_TEST(const_local_safe_view_alias_3427,
           var_view_array_backing_and_wrapper_reclaimed_spec_28_9_28_11) {
    BEZEL_EXPECT_EQ(run(program("var")), "60300\n");
}

// ── THE FIXED HALF — under the STRICT oracle. fix #3640 (Register iterable
// disposal for §21.1 const views) closed the residual wrapper-frame leak
// `Emitter::registerIterableSourceDisposal` had on the LocalConstDecl arm
// (the LocalDecl arm of `lowerStmt` already disposed; the LocalConstDecl arm
// silently dropped the registration, leaking ~6368 B / 199 allocs — exactly
// #3392's 32-B borrowing wrapper frame measurement). With #3640 landed the
// same program is now fully LSan-clean: this row's oracle reverts from a
// threshold-byte assertion (`r.ran == false`, `leaked < 10000`) to the plain
// strict pass (`r.ran == true`) — the FAIL-WITHOUT/PASS-WITH proof moved to
// `tests/codegen/const_local_enumerable_view_disposal_3640_test.cpp`'s
// `const_ienumerable_view_wrapper_reclaimed_spec_21_1_29_11` row, which is
// the load-bearing regression test for this exact shape. `stdoutText` is
// still asserted directly: an unsound over-eager reclaim of the array while
// `v` is still iterating it would either crash before the loop finishes
// (wrong/truncated stdout) or show up as a heap-use-after-free/double-free
// — both observed by the strict oracle.
// per §21.1, §28.9, §28.11 (issue #3427; residual closed by #3640)
BEZEL_TEST(const_local_safe_view_alias_3427,
           const_view_array_backing_and_wrapper_reclaimed_spec_21_1_28_9) {
    auto r = runAotChecked(program("const"));
    BEZEL_EXPECT_TRUE(r.ran);                // fix #3640: residual closed
    BEZEL_EXPECT_EQ(r.stdoutText, "60300\n");
}

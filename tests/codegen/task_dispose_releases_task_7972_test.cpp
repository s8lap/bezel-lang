// fix #7972 (§28.2 / §28.3 / §30.5 / §30.3, v0.353) — an explicit
// `Dispose()` on a TASK receiver must DISPOSE THE TASK, not just drop the
// carrier: `t?.Dispose()` on a `Task<int>?` local leaked the whole task.
//
// ── THE DEFECT ──────────────────────────────────────────────────────────────
//
// `Emitter::emitExplicitDisposeChain` (src/codegen/LlvmBackend_Calls2.cpp) is
// the ONE dispatch chain both spellings of an explicit dispose go through
// (`x.Dispose()` — lowerCallDispatch — and `x?.Dispose()` — lowerNullCondCall,
// which calls the chain for the non-null edge). Its arm chain is type-first
// and ends in the vtable-less-builtin arm:
//
//     } else if (common::isVtableLessBuiltinClassName(builtinTypeName(recvTy))) {
//         ...
//         emitVtablelessBuiltinRelease(builtinTypeName(recvTy), recv);
//
// `common::isVtableLessBuiltinClassName` DOES list "Task" (a `bz_task_t*` is a
// runtime struct with no Bezel vtable at word 0), so a Task receiver REACHED
// that arm — but `Emitter::classifyVtablelessBuiltinRelease`
// (src/codegen/LlvmBackend_Disposal.cpp) has NO Task entry, so
// `emitVtablelessBuiltinRelease` returned false and emitted NOTHING. That is
// the trade #2874/#2823 record ("a bounded leak in place of corrupted control
// flow") — but for the Task family it is not bounded: the task struct is
// 184 bytes and its result buffer another 4.
//
// The leak is then made PERMANENT by the chain's own §28.2 exactly-once
// bookkeeping: the chain's single exit nulls the receiver's slot
// (`nullExplicitlyDisposedBuiltinSlot`), so the §28.3 scope-end auto-drop
// reloads null and discharges nothing too. MEASURED with BEZEL_DUMP_IR on the
// pre-fix tree (`build/bezelc`, base c05c35bf):
//
//     %task.from_result = call ptr @bz_task_from_result(ptr %from_result.slot, i64 4)
//     store ptr %task.from_result, ptr %0
//     ...
//     bidisp.nn:                       ; the vtable-less-builtin arm
//       br label %bidisp.cont          ; emitVtablelessBuiltinRelease: NOTHING
//     bidisp.cont:
//       store ptr null, ptr %0          ; the chain's exit nulls the slot
//     ncdisp.end:
//       %4 = load ptr, ptr %0           ; the scope-end auto-drop reloads NULL
//       %5 = icmp eq ptr %4, null
//       br i1 %5, label %task.cont, label %task.faf
//
// ⇒ MEASURED PRE-FIX, under LeakSanitizer (`bezelc --sanitize=address`):
//     Direct leak of 184 byte(s) in 1 object(s)
//     Indirect leak of 4 byte(s) in 1 object(s)
//       #1 bz_task_from_result src/runtime/bezel_runtime_concurrency.cpp:1290
//     SUMMARY: AddressSanitizer: 188 byte(s) leaked in 2 allocation(s).
//
// The same 188 bytes leak for the DOTTED spelling (`t.Dispose()`), for a
// non-nullable receiver (`var t := Task.FromResult(2); t.Dispose();`), and for
// a DOUBLE dispose (`t?.Dispose(); t?.Dispose();`).
//
// ── THE FIX ────────────────────────────────────────────────────────────────
//
// §30.5: disposing a task is ABANDONEMENT — "the task's Dispose() runs but
// does not block waiting for completion" (stdlib §6.7) — and §30.3 gives the
// task a `destructor;` whose body is the runtime's continuation cleanup. The
// established protocol for that is `Emitter::emitTaskDisposal`
// (src/codegen/LlvmBackend_Disposal.cpp): §30.9's fault-object ownership
// preamble, then the per-`T` result disposer for an OwningTask<T>, then
// `bz_task_fire_and_forget` — whose already-done fast path calls
// `bz_task_free`, i.e. it reclaims BOTH the task struct and its result buffer.
// The fix routes a Task receiver of the explicit-dispose chain to that
// protocol, BEFORE the vtable-less-builtin arm (which has no Task release).
//
// The chain's §28.2 slot-nulling then does its job: the scope-end auto-drop
// reloads null and discharges nothing, so the task is disposed EXACTLY ONCE
// (§28.2: "the first call performs the disposal; subsequent calls are
// no-ops") — which is what makes the double-dispose row below a no-op rather
// than a double free.
//
// ── THE ORACLE ─────────────────────────────────────────────────────────────
//
// A row that only asserts a VALUE cannot observe a leak (the brief's rule),
// so every leak row here runs the generated program under LeakSanitizer with
// the harness's opt-in leak oracle in STRICT mode
// (BEZEL_AOT_DETECT_LEAKS=strict, issue #4638) and asserts the oracle's OWN
// record: `LeakExpectation::count` is the number of leak reports the harness
// saw. `count == 0` is the positive claim "the task was reclaimed"; the
// pre-fix tree reports 1 (188 bytes / 2 allocations), so the rows are RED
// without the fix.
//
// The CONTROL rows (the null task, and the known-positive leak) are disclosed
// in the lane report: they PASS on the pre-fix tree by construction, and they
// exist to prove the oracle is ARMED (a leak oracle nobody has seen fail
// cannot distinguish "no leak" from "not looking").

#include <cstdlib>
#include <string>

#include "support/aot_support.hpp"    // runAot — AOT test harness
#include "support/aot_result_asserts.hpp"
#include "support/force_sanitize.hpp"
#include "test_harness.hpp"

namespace {

using bezel::test::JitResult;

// BEZEL_AOT_DETECT_LEAKS is a process-global knob; save/restore it so the
// rows are order- and ambient-independent (the #4638 test's own discipline).
class LeakEnv {
public:
    explicit LeakEnv(const char* value) {
        const char* prev = std::getenv("BEZEL_AOT_DETECT_LEAKS");
        had_ = prev != nullptr;
        if (had_) prev_ = prev;
        if (value) ::setenv("BEZEL_AOT_DETECT_LEAKS", value, /*overwrite=*/1);
        else       ::unsetenv("BEZEL_AOT_DETECT_LEAKS");
    }
    ~LeakEnv() {
        if (had_) ::setenv("BEZEL_AOT_DETECT_LEAKS", prev_.c_str(), 1);
        else      ::unsetenv("BEZEL_AOT_DETECT_LEAKS");
    }
    LeakEnv(const LeakEnv&) = delete;
    LeakEnv& operator=(const LeakEnv&) = delete;
private:
    bool had_ = false;
    std::string prev_;
};

bezel::codegen::CodegenOptions asanOpts() {
    bezel::codegen::CodegenOptions o{};
    o.sanitizeAddress = true;
    return o;
}

// The harness SKIPs (loudly, counted) when clang is absent; a row that
// asserted through that would be asserting on an unarmed oracle.
bool skipped(const JitResult& r) {
    return bezel::test::diagnosticMessageContains(r, "SKIP");
}

// ── the programs ────────────────────────────────────────────────────────────
//
// Each one prints a marker line, so the stdout assertion is a POSITIVE claim
// about the program's observable behaviour, not a side effect of "it ran".

// The issue's own shape: a `Task<int>?` local, disposed through the
// null-conditional spelling.
constexpr const char* kNullCondDispose =
    "module M; import System; "
    "class C public static method Main(): int "
    "  var t: Task<int>? := Task.FromResult(2); "
    "  t?.Dispose(); "
    "  Console.WriteLine(\"null-cond-dispose-done\"); "
    "  return 0; "
    "end Main; end C;";

// The DOTTED spelling over the same nullable receiver — the same chain, the
// same missing release.
constexpr const char* kDottedDispose =
    "module M; import System; "
    "class C public static method Main(): int "
    "  var t: Task<int>? := Task.FromResult(2); "
    "  t.Dispose(); "
    "  Console.WriteLine(\"dotted-dispose-done\"); "
    "  return 0; "
    "end Main; end C;";

// The DOTTED spelling over a NON-nullable receiver (`var t := …` — the
// inferred type), i.e. the shape the R37 review's probe used.
constexpr const char* kDottedDisposeInferred =
    "module M; import System; "
    "class C public static method Main(): int "
    "  var t := Task.FromResult(2); "
    "  t.Dispose(); "
    "  Console.WriteLine(\"dotted-inferred-dispose-done\"); "
    "  return 0; "
    "end Main; end C;";

// DOUBLE dispose: the second call must be a no-op (§28.2 exactly-once), not
// a double free. Pre-fix this row is RED through the LEAK, not through a
// double free — nothing was freed at all — so the row's post-fix claim is
// "exactly one reclaim, and no sanitizer error".
constexpr const char* kDoubleDispose =
    "module M; import System; "
    "class C public static method Main(): int "
    "  var t: Task<int>? := Task.FromResult(2); "
    "  t?.Dispose(); "
    "  t?.Dispose(); "
    "  Console.WriteLine(\"double-dispose-done\"); "
    "  return 0; "
    "end Main; end C;";

// The NULL case: no task ⇒ no disposal, no crash. This row PASSES on the
// pre-fix tree (the `?.` guard skips) — it is a CONTROL.
constexpr const char* kNullTaskDispose =
    "module M; import System; "
    "class C public static method Main(): int "
    "  var t: Task<int>? := null; "
    "  t?.Dispose(); "
    "  Console.WriteLine(\"null-task-dispose-done\"); "
    "  return 0; "
    "end Main; end C;";

// The KNOWN POSITIVE: 500 membership queries against `Array<Object>`, each
// one §8.6-boxes the `int` query and the box is never released (issue #4619,
// a declined residual). 24 B per box, 500 boxes ⇒ 12000 B in 500 allocations —
// the exact figures #4638's own test measured for this program.
constexpr const char* kKnownLeak500 =
    "module M; class C public static method Main(): int "
    "  var a: Array<Object> := [1, 2]; "
    "  var hits: int := 0; "
    "  for var i in 1..500 do "
    "    if a.Contains(i) then hits := hits + 1; end; "
    "  end; "
    "  return hits; "
    "end Main; end C;";

}  // namespace

// ── 1. the issue's own shape: `t?.Dispose()` must release the task ─────────
//
// RED without the fix: the harness's leak oracle sees ONE report (188 bytes
// in 2 allocations) and the run is marked failed, so `exp.count == 0`,
// `r.ran` and the exit code all fail here.
BEZEL_TEST(task_dispose_releases_task_7972,
           null_cond_dispose_releases_the_task) {
    LeakEnv env("strict");
    bezel::test::aot_detail::LeakExpectation exp;
    auto r = bezel::test::runAot(kNullCondDispose, asanOpts());
    if (skipped(r)) return;
    BEZEL_EXPECT_EQ(exp.count, std::size_t{0});
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("null-cond-dispose-done\n"));
}

// ── 2. the dotted spelling over the same receiver ──────────────────────────
BEZEL_TEST(task_dispose_releases_task_7972,
           dotted_dispose_releases_the_task) {
    LeakEnv env("strict");
    bezel::test::aot_detail::LeakExpectation exp;
    auto r = bezel::test::runAot(kDottedDispose, asanOpts());
    if (skipped(r)) return;
    BEZEL_EXPECT_EQ(exp.count, std::size_t{0});
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("dotted-dispose-done\n"));
}

// ── 3. the dotted spelling over an INFERRED (non-nullable) receiver ────────
BEZEL_TEST(task_dispose_releases_task_7972,
           dotted_dispose_of_an_inferred_task_releases_the_task) {
    LeakEnv env("strict");
    bezel::test::aot_detail::LeakExpectation exp;
    auto r = bezel::test::runAot(kDottedDisposeInferred, asanOpts());
    if (skipped(r)) return;
    BEZEL_EXPECT_EQ(exp.count, std::size_t{0});
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("dotted-inferred-dispose-done\n"));
}

// ── 4. double dispose: exactly one reclaim, no double free ─────────────────
//
// `r.ran` is the sanitizer's verdict as well: an ASan double free aborts the
// program, so a row that saw one would report `r.ran == false`.
BEZEL_TEST(task_dispose_releases_task_7972,
           double_dispose_is_not_a_double_free) {
    LeakEnv env("strict");
    bezel::test::aot_detail::LeakExpectation exp;
    auto r = bezel::test::runAot(kDoubleDispose, asanOpts());
    if (skipped(r)) return;
    BEZEL_EXPECT_EQ(exp.count, std::size_t{0});
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("double-dispose-done\n"));
}

// ── 5. CONTROL (passes pre-fix): a null task is a no-op ────────────────────
//
// The `?.` null-guard skips the disposal; there is nothing to reclaim, so the
// oracle sees no leak. This row exists so that rows 1-4 cannot be satisfied
// by a compiler that simply never disposes anything.
BEZEL_TEST(task_dispose_releases_task_7972, null_task_dispose_is_a_no_op) {
    LeakEnv env("strict");
    bezel::test::aot_detail::LeakExpectation exp;
    auto r = bezel::test::runAot(kNullTaskDispose, asanOpts());
    if (skipped(r)) return;
    BEZEL_EXPECT_EQ(exp.count, std::size_t{0});
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("null-task-dispose-done\n"));
}

// ── 6. CONTROL (passes pre-fix): the leak oracle IS armed ─────────────────
//
// The known positive of issue #4619 (500 unreleased §8.6 boxes). The oracle
// must FIRE on it, with the exact byte/alloc counts — otherwise rows 1-4's
// `count == 0` would be satisfied by an oracle that never looks.
BEZEL_TEST(task_dispose_releases_task_7972,
           the_leak_oracle_fires_on_a_known_leak) {
    LeakEnv env("strict");
    bezel::test::aot_detail::LeakExpectation exp;
    auto r = bezel::test::runAot(kKnownLeak500, asanOpts());
    if (skipped(r)) return;
    BEZEL_EXPECT_EQ(exp.count, std::size_t{1});
    BEZEL_EXPECT_TRUE(exp.lastFailing);
    BEZEL_EXPECT_EQ(exp.lastBytes, 12000ull);
    BEZEL_EXPECT_EQ(exp.lastAllocs, 500ull);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    // The SPECIFIC diagnostic, not merely "some error": a compile reject, a
    // crash and a timeout all set hasErrors() too, and every one of them would
    // satisfy a weaker assertion while proving nothing about leak detection.
    //
    // `runAot`'s `ran` is false for FOUR outcomes (issue #3892): a genuine
    // compile reject, the compiler itself crashing, the program dying from a
    // runtime signal, and a timeout or a forced-ASan report. The bare
    // `BEZEL_EXPECT_FALSE(r.ran)` above therefore cannot tell the leak
    // oracle's abort from any of the other three — and it passes even when
    // the compiled program SEGFAULTS. These two pins name the REASON the
    // program did not run: the leak oracle fired on the known leak.
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, "LeakSanitizer fired"));
    BEZEL_EXPECT_TRUE(
        bezel::test::diagnosticMessageContains(r, "byte(s) leaked in"));
}

// Regression pin for issue #8600 — §28.2 checked disposal mode: an explicit
// `Dispose()` of a settled `Task` local hangs `Main` at scope end.
//
// §28.2 (`Use after disposal; checked disposal mode`), §28.3 (the
// "explicitly-disposed" post-state and the scope-end discharge), §30.5 (result
// lifetime and task disposal) / stdlib §6.7 (`Task` static helpers), v0.357.
//
// THE RULE. §28.3's glossary entry for the "explicitly-disposed" post-state:
// "the binding after a site-announced kill that moves no obligation … Scope-end
// machinery SKIPS ITS DISPOSAL — the block is still freed at the holder's
// release point, §28.2's compiler-inserted free — and a use the kill dominates
// is §28.2's use-after-dispose error, a repeated explicit disposal its
// warning". And §28.2: "Dispose() on any class instance is idempotent: the
// first call performs the disposal; subsequent calls are no-ops."
//
// So an explicit `t.Dispose()` OWNS the discharge, and the §28.3 scope-end
// discharge of the same binding must find nothing to do. The compiler
// discharges that hand-off by NULLING the receiver's slot at the explicit
// call, so the scope-end reload finds null.
//
// THE DEFECT (checked-disposal mode only). The slot-null is suppressed in
// checked mode — `Emitter::nullExplicitlyDisposedBuiltinSlot` returns early
// when `checkedDisposal_` is set — because for the CLASS family that is right:
// a checked-mode release QUARANTINES the block (the disposed flag stays
// readable), so the scope-end's reload of the same pointer re-runs an
// idempotent release on a block that is still there. The TASK family is not
// such a release: its disposal protocol is
// `bz_task_fire_and_forget[_checked]` → `bz_task_free` → `bz_task_destroy` →
// `delete task`, so the task struct (and the std::mutex inside it) is GONE the
// moment the explicit `t.Dispose()` runs, in BOTH modes.
//
// The scope-end task discharge then called `bz_task_is_error(t)` on that freed
// struct, which locks the DESTROYED mutex. MEASURED (gdb on the hung process):
//
//   #0  __lll_lock_wait ()
//   #1  pthread_mutex_lock@@GLIBC_2.2.5 ()
//   #2  bz_task_is_error ()
//   #3  bz$async$M.Program$Main$Resume ()
//   #4  domain_drain(bz_domain_t*) ()
//   #5  BzScheduler::run_worker() ()
//
// an infinite spin (the freed chunk's first bytes are glibc's tcache `next`
// pointer, so the destroyed mutex's `__lock` word is non-zero). The program
// printed "done" and then hung forever. The same use-after-free is what a
// REPEATED `t.Dispose()` took (the second call reloaded the still-set slot).
//
// THE FIX. The Task arm of the explicit-`Dispose()` chain nulls the receiver's
// slot in BOTH modes (its release frees, so the checked-mode "keep the slot"
// policy does not apply to it) and null-guards the receiver (the repeated
// `Dispose()` reloads the nulled slot, and every other arm of that chain
// already had the guard — without it a repeated `t.Dispose()` SEGFAULTED on a
// null receiver even in unchecked mode).
//
// THE ROWS. Seven rows are RED on the base (the issue's own repro in checked
// mode, the same under the memory net, the repeated disposal in checked mode,
// the `using var` spelling, the `!` spelling, the still-running task, and the
// repeated disposal in the DEFAULT mode); one row is the suite's boundary
// control (green on both arms — the scope-end discharge must still free a
// task that was never explicitly disposed, which only the LeakSanitizer net
// can see).
//
// The oracles: `runAotCheckedDisposal` is §28.2's checked-disposal MODE (the
// mode the defect lives in); `runAotChecked` is the generated-code
// AddressSanitizer + LeakSanitizer memory net, and is run with the
// checked-disposal bit set so the memory net observes the same mode.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedDisposal;

namespace {

// The issue's own repro, verbatim (issue #8600). Built with
// `bezelc --checked-disposal`: it prints "done" and then HANGS (rc=124 under
// `timeout 10`), while the same program without `--checked-disposal` prints
// "done" and exits 0.
constexpr const char* kIssueRepro =
    "module M;\n"
    "public class Program\n"
    "  public static async method W(): Task\n"
    "    await Task.Delay(1);\n"
    "  end W;\n"
    "  public static async method Main(): Task\n"
    "    var t := Program.W();\n"
    "    await t;\n"
    "    t.Dispose();\n"
    "    Console.WriteLine(\"done\");\n"
    "  end Main;\n"
    "end Program;\n";

// §28.2 idempotence: a repeated explicit disposal is a no-op (the compiler
// warns about it; the runtime must not touch the freed task a second time).
constexpr const char* kDoubleDispose =
    "module M;\n"
    "public class Program\n"
    "  public static async method W(): Task\n"
    "    await Task.Delay(1);\n"
    "  end W;\n"
    "  public static async method Main(): Task\n"
    "    var t := Program.W();\n"
    "    await t;\n"
    "    t.Dispose();\n"
    "    t.Dispose();\n"
    "    Console.WriteLine(\"done\");\n"
    "  end Main;\n"
    "end Program;\n";

// The `using var` spelling: the explicit disposal and the §28.3 scope-end
// auto-drop both reach one receiver.
constexpr const char* kUsingVarExplicitDispose =
    "module M;\n"
    "public class Program\n"
    "  public static async method W(): Task\n"
    "    await Task.Delay(1);\n"
    "  end W;\n"
    "  public static async method Main(): Task\n"
    "    using var t := Program.W();\n"
    "    await t;\n"
    "    t.Dispose();\n"
    "    Console.WriteLine(\"done\");\n"
    "  end Main;\n"
    "end Program;\n";

// The `!` (non-null-forgiving) spelling of the same shape: a nullable Task
// local, disposed through `t!.Dispose()`. The `!` narrows nullability but does
// not change WHICH object the receiver denotes, so the slot to null is the
// operand's — without that unwrap the pointer stayed in the slot and the
// §28.3 scope-end discharge re-read the freed task struct (the same hang, in
// BOTH modes).
constexpr const char* kBangDispose =
    "module M;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var t: Task<int>? := Task.FromResult(1);\n"
    "    t!.Dispose();\n"
    "    Console.WriteLine(\"done\");\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// A task that is STILL RUNNING at the explicit disposal: the disposal is an
// ABANDONMENT (stdlib §6.7 / §30.5), so the task struct is freed by the
// abandonment's completion continuation. The scope-end discharge must not
// register a SECOND continuation for the same task (two continuations = two
// frees of one task struct). The program waits long enough for the task to
// settle, so a second continuation would fire inside this run.
constexpr const char* kStillRunningDispose =
    "module M;\n"
    "public class Program\n"
    "  public static async method W(): Task\n"
    "    await Task.Delay(50);\n"
    "  end W;\n"
    "  public static async method Main(): Task\n"
    "    var t := Program.W();\n"
    "    t.Dispose();\n"
    "    Console.WriteLine(\"done\");\n"
    "    await Task.Delay(400);\n"
    "  end Main;\n"
    "end Program;\n";

// CONTROL (green on the base and on the fix): the scope-end discharge of a
// `using var` task with NO explicit disposal still runs and still frees the
// task struct — so the memory net sees no leak. A fix that skipped the
// scope-end discharge outright would leak the task struct and turn this row
// red (LeakSanitizer).
constexpr const char* kScopeEndOnly =
    "module M;\n"
    "public class Program\n"
    "  public static async method W(): Task\n"
    "    await Task.Delay(1);\n"
    "  end W;\n"
    "  public static async method Main(): Task\n"
    "    using var t := Program.W();\n"
    "    await t;\n"
    "    Console.WriteLine(\"done\");\n"
    "  end Main;\n"
    "end Program;\n";

// §28.2 checked-disposal MODE (not the sanitizer).
JitResult checked(const std::string& src) {
    return runAotCheckedDisposal(src);
}

// §28.2 checked-disposal MODE under the generated-code ASan/LSan memory net.
JitResult checkedAsan(const std::string& src) {
    bezel::codegen::CodegenOptions opts;
    opts.checkedDisposal = true;
    return runAotChecked(src, opts);
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. THE ISSUE'S REPRO (checked-disposal mode). Before the fix the program
//    printed "done" and then hung: the scope-end discharge called
//    `bz_task_is_error` on the task struct the explicit `t.Dispose()` had
//    already freed, and the destroyed mutex's lock never came back.
//    A hang is a MEASURED failure: the AOT harness kills the program at its
//    run deadline and reports ran=false.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_task_explicit_dispose_scope_end_8600,
           issue_repro_settled_task_dispose_then_scope_end_28_2) {
    JitResult r = checked(kIssueRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("done\n"));
}

// ---------------------------------------------------------------------------
// 2. The same repro under the generated-code ASan/LSan memory net, with the
//    checked-disposal bit set: no use-after-free in the emitted code and no
//    leak of the task struct the explicit disposal freed.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_task_explicit_dispose_scope_end_8600,
           issue_repro_settled_task_dispose_then_scope_end_under_asan_28_2) {
    JitResult r = checkedAsan(kIssueRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("done\n"));
}

// ---------------------------------------------------------------------------
// 3. §28.2 idempotence: a REPEATED explicit disposal is a no-op. Before the
//    fix the second `t.Dispose()` reloaded the slot the first one had left
//    pointing at the freed task struct (checked mode: the hang of row 1;
//    unchecked mode: a null-deref SEGV, because the Task arm of the
//    explicit-Dispose chain had no null guard).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_task_explicit_dispose_scope_end_8600,
           repeated_explicit_dispose_is_a_noop_28_2) {
    JitResult r = checked(kDoubleDispose);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("done\n"));
}

// ---------------------------------------------------------------------------
// 4. The `using var` spelling: the explicit disposal and the §28.3 scope-end
//    auto-drop both reach one receiver. Before the fix the auto-drop reloaded
//    the freed task struct and hung.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_task_explicit_dispose_scope_end_8600,
           using_var_task_explicit_dispose_then_scope_end_28_2) {
    JitResult r = checked(kUsingVarExplicitDispose);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("done\n"));
}

// ---------------------------------------------------------------------------
// 5. The `!` (non-null-forgiving) spelling of the same shape. Before the fix
//    the `!` receiver's slot was not the operand's, so the pointer stayed in
//    it and the §28.3 scope-end discharge re-read the freed task struct — the
//    hang of row 1, in BOTH modes.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_task_explicit_dispose_scope_end_8600,
           bang_spelling_task_explicit_dispose_then_scope_end_28_2) {
    JitResult r = checked(kBangDispose);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("done\n"));
}

// ---------------------------------------------------------------------------
// 6. An explicit disposal of a STILL-RUNNING task is an ABANDONMENT
//    (stdlib §6.7 / §30.5): the task struct is freed by the abandonment's
//    completion continuation. The scope-end discharge must not register a
//    SECOND continuation for the same task. Before the fix it did, and the
//    program hung.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_task_explicit_dispose_scope_end_8600,
           still_running_task_explicit_dispose_then_scope_end_28_2) {
    JitResult r = checked(kStillRunningDispose);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("done\n"));
}

// ---------------------------------------------------------------------------
// 7. The SAME repeated disposal in the DEFAULT (unchecked) mode. Before the
//    fix the second `t.Dispose()` reloaded the slot — which the unchecked
//    mode's chain exit HAD already nulled — and the Task arm of the
//    explicit-Dispose chain had no null guard, so the runtime was called with
//    a NULL task: MEASURED on the base, the program dumped core (SIGSEGV in
//    `bz_task_is_error(NULL)`). §28.2's idempotence rule holds in BOTH modes.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_task_explicit_dispose_scope_end_8600,
           repeated_explicit_dispose_in_default_mode_is_a_noop_28_2) {
    JitResult r = runAot(kDoubleDispose);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("done\n"));
}

// ---------------------------------------------------------------------------
// CONTROL (green before the fix and after it — the suite's ONE boundary
//    control, and a check a conformance case cannot express: the
//    LeakSanitizer net): a `using var` task with NO explicit disposal is
//    still discharged at scope end, and the task struct is still freed — the
//    memory net sees no leak. A fix that SKIPPED the scope-end discharge
//    altogether (e.g. "a task's scope-end discharge is unnecessary now that
//    the explicit Dispose frees the struct") would leak the task struct and
//    turn this row red (LeakSanitizer).
//
//    The DEFAULT-mode twin of the issue's repro (the same program without
//    `--checked-disposal`, which is green on the base) is NOT a gtest row:
//    it is a base-green row, so it goes to the conformance corpus — see
//    `<probe dir>/corpus-candidates/`.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_task_explicit_dispose_scope_end_8600,
           control_scope_end_discharge_still_frees_the_task_28_2) {
    JitResult r = checkedAsan(kScopeEndOnly);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("done\n"));
}

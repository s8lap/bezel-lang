// Codegen / runtime — issue #1905 (spec §28.3 / §30.5): an unbound
// `Task` returned by a call and synchronously drained via `.Wait()` is an
// "unbound owned temporary" (§28.3) whose lifetime ends at the full
// expression; a Task-family call mints a caller-owned frame (§30.5), and
// `Wait()` only borrows that frame, so the synchronous use site must run
// the same task-disposal protocol that a `using var` does at scope end.
// Before the fix the call's `bz_task_t` was orphaned — codegen dropped the
// pointer right after `bz_task_await_sync`; LSan would report 184 B at -O0
// (the size of `bz_task_t`), masked at the historic O2 codegen default.
//
// Spec authority:
//   - §28.3 (Unbound owned temporaries): "Such a temporary is disposed at
//     the end of the full expression in which it is produced, after every
//     use of it within that expression has completed."
//   - §30.5 (Task-family calls produce owned frames): "an async method's
//     state-machine frame holds its plain reference-typed arguments'
//     borrows until settlement"; the call mints an owned frame, the
//     consumer's `Wait()` only borrows it.
//   - §30.3: `Wait()` is the synchronous escape hatch; inside a domain it
//     is rejected (§30.3 v0.162 / #1362), so the issue's repro is the
//     boundary case — a non-async `Main` calling `.Wait()` from outside
//     any domain.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

int run(const std::string& src) {
    JitResult r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// Bump a static counter every time an `async` method is entered; the
// paired exit counter drops to zero iff no Task frame leaks. (The leak
// detector LSan would catch it for -O0, but the project's default
// codegen is the O2 sweep that historically masks it — exactly the
// claim of #1905.)
const char* kProbe =
    "class Probe "
    "  public static entered: int := 0; "
    "  public static exited: int := 0; "
    "  public static async method Tick(n: int): Task "
    "    Probe.entered := Probe.entered + 1; "
    "    await Task.Delay(0); "
    "    Probe.exited := Probe.exited + 1; "
    "  end Tick; "
    "end Probe; ";

}  // namespace

// #1905 exact repro: a non-async `Main` calls `Probe.Tick(7).Wait()`.
// Wait is legal from the synchronous prefix; the receiver is an unbound
// task temporary. Before the fix the 184 B `bz_task_t` leaked; after
// the fix it is reclaimed via the standard task-disposal protocol. The
// observable (entered == exited) confirms the function actually ran
// (rules out a hang, a fire-and-forget miscompile, or a no-op).
BEZEL_TEST(anon_task_wait_free_1905,
           run_enter_exit_eq_spec_30_5) {
    const std::string src =
        std::string("module M; ") + kProbe +
        "class C "
        "  public static method Main(): int "
        "    Probe.Tick(7).Wait(); "
        "    return Probe.entered - Probe.exited; "  // 0 iff balanced
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

// #1905 control: the same source shape, but bound to a `using var` first.
// The using-var path was already correctly disposed at scope end before
// the fix and must remain so. Verifies the Wait-arg fix did not
// regress a value-borrowing use (e.g. a prior Wait over a `var` task
// reaching in here).
BEZEL_TEST(anon_task_wait_free_1905,
           using_var_anon_wait_unchanged_control_spec_30_5) {
    const std::string src =
        std::string("module M; ") + kProbe +
        "class C "
        "  public static method Main(): int "
        "    using var t := Probe.Tick(9); "
        "    t.Wait(); "
        "    return Probe.entered - Probe.exited; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

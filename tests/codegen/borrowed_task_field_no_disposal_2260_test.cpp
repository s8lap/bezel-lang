// Codegen — issue #2260 (HIGH, double-free): a NON-`owned` (borrowed)
// Task-family FIELD must not be disposed by its holder.
//
// Spec basis (v0.226):
//   §28.5  an owned field owns its value; a non-`owned` reference field is a
//          BACK-REFERENCE / borrow and the holder has no disposal obligation
//          for it.
//   §22.12 rule 2(ii) storage reads lend; the mint CALL is the owner.
//   §28.3  "Task-family calls produce owned frames" (v0.190, SB-01) — the one
//          owner of a task frame is the call that minted it (or the binding it
//          was transferred to), never a borrowing field.
//   §30.5  disposing a task runs the full drop protocol (fault-object prelude
//          + per-T result_disposer + bz_task_fire_and_forget[_checked]).
//   §22.10 a captured `owned` primary-constructor parameter is "captured as a
//          compiler-generated `owned` private field, disposed by the generated
//          `Dispose()` exactly like a declared owned field (§28.4)".
//
// THE DEFECT. emitOwnedFieldDisposals' Task arm (LlvmBackend_Disposal.cpp:497)
// classified on `taskFamilyTypeFromField(fs)`, which tested only
// `isTaskTypeName(rt)` — it did NOT test `hasOwnedModifier(fs)`. Every sibling
// arm in that same `else if` chain gates on `owned`. The other caller of the
// helper, isOwnedTaskField (the field-STORE path), compensated with its own
// pre-check; the DISPOSAL path did not. So a borrowed Task field was disposed
// by its holder AND by its real owner:
//
//   Site A (spurious): LlvmBackend_Disposal.cpp:497 -> :686 emitOwnedTaskDisposal
//                      -> emitTaskDisposal (:268) -> bz_task_fire_and_forget
//                      (:406/:411) -> bz_task_free.
//   Site B (real):     the owner's scope-end drop, LlvmBackend_Disposal.cpp:2918
//                      (`if (it->task)`) -> the same sequence.
//
// The fix moves the `owned` test INTO taskFamilyTypeFromField
// (LlvmBackend_Setup.cpp) so both callers share one predicate.
//
// WHAT FAILS WITHOUT THE FIX. Every task entry point takes `task->mtx` first
// (bz_task_is_error / bz_task_fire_and_forget / TaskCompletionSource's
// destructor all lock it), so site B locks the mutex of the block site A
// already `delete`d. The freed std::mutex reads as permanently held, and the
// program DEADLOCKS at the second disposal — reproduced here at 100% under
// both a plain and an ASan build. runAotChecked reports that as
// `ran=false` ("program timed out"), which the `run` wrapper below turns into
// the -999 sentinel; post-fix the same programs run to completion AND are
// verified LSan-clean, so the fix cannot have traded the double free for a
// leak. (The pre-fix symptom is a hang, not an ASan headline, only because
// the UAF happens inside the non-instrumented C++ runtime and the very first
// freed-memory access is the mutex lock — the process never reaches the second
// `delete`. Named sites A and B above are the two frees.)

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// The oracle is the HARNESS verdict, not the program's exit code: a sanitizer
// report, a crash, or a run that never terminates all surface as ran=false and
// collapse to -999 here.
int run(const std::string& src) {
    JitResult r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// Cell: ctor ++ / destructor -- static counter — the dispose-exactly-once
// oracle for the OwningTask<Cell> result shapes below.
const char* kCell =
    "class Cell public static n: int := 0; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; ";

}  // namespace

// ---------------------------------------------------------------------------
// #2260 — the traced double free. The borrowing shape is the one the accepted
// lit test tests/lit/ownership/task_family_borrow_return_1786__reject.bzl:31-32
// uses as its scaffolding (and sema_builtin__task_t_resolves.bzl:9 pins as
// accepted): a class holding a plain `Task<T>` field assigned from a plain
// (non-`owned`) constructor parameter. `t` is the sole owner; `Cache` only
// borrows.
//
// Fails without the fix: Cache's generated disposer classified `_cachedTask`
// as an owned task field and ran the full drop protocol on it, freeing the
// task at the inner `do` block's end; `t`'s own scope-end drop then locked the
// freed task's mutex and the process hung forever -> runAotChecked timeout ->
// -999 != 0. Passes with the fix: `Cache$DisposeCore` emits no bz_task_* call
// at all and the program exits 0, LSan-clean.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_borrowed_task_field_2260,
           borrowed_task_field_not_disposed_by_holder_spec_28_5_22_12) {
    // per §28.5 / §22.12 rule 2(ii) / §28.3 (v0.190, SB-01)
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class Cache "
        "  private _cachedTask: Task<int>; "
        "  public constructor(t: Task<int>) _cachedTask := t; end constructor; "
        "  public method Peek(): int => 7; "
        "end Cache; "
        "class App "
        "  public static async method Tick(i: int): Task<int> "
        "    return i; "
        "  end Tick; "
        "  public static method Main(): int "
        "    using var t := App.Tick(1); "
        "    do "
        "      using var c := new Cache(t); "
        "      Console.WriteLine(\"peek=\" + c.Peek().ToString()); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end App;"), 0);
}

// ---------------------------------------------------------------------------
// #2260 — the same defect reached through the OTHER borrow source the spec
// names: a task read out of storage. §30.11 / stdlib §6.7 — "Stored tasks are
// exposed as properties (TaskCompletionSource<T>.Task), whose reads are
// borrows of the source's storage." The source, not the borrowing field, is
// the one disposer (its destructor "disposes the produced task").
//
// Fails without the fix: Cache's disposer freed the source's task at the inner
// block's end; the source's own destructor then locked the freed task and hung
// -> -999 != 0. This cell exists so the fix is pinned against BOTH borrow
// sources (a using-var owner and stored-task storage), not just the shape the
// issue happened to trace.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_borrowed_task_field_2260,
           borrowed_task_field_from_stored_task_property_spec_30_11) {
    // per §30.11 / stdlib §6.6 / §22.12 rule 2(ii)
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class Cache "
        "  private _cachedTask: Task<int>; "
        "  public constructor(t: Task<int>) _cachedTask := t; end constructor; "
        "  public method Peek(): int => 7; "
        "end Cache; "
        "class App "
        "  public static method Main(): int "
        "    using var tcs := new TaskCompletionSource<int>(); "
        "    tcs.SetResult(5); "
        "    do "
        "      using var c := new Cache(tcs.Task); "
        "      Console.WriteLine(\"peek=\" + c.Peek().ToString()); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end App;"), 0);
}

// ---------------------------------------------------------------------------
// #2260 — the CONSTRUCTION-THROW disposal path. emitOwnedFieldDisposals has
// three callers, all of which shared the defective classifier:
//   LlvmBackend_Disposal.cpp:208   the generated Dispose() core,
//   LlvmBackend_Setup.cpp:6604     inlineCtorBody's §22.9 partially-constructed
//                                  cleanup (explicit ctor),
//   LlvmBackend_Exprs2.cpp:4284    the no-explicit-ctor field-init cleanup.
// §22.9: a throwing construction disposes the owned fields whose initialization
// completed, in reverse declaration order — a BORROWED field is not one of
// them.
//
// The shape drives the THIRD site: `Cache` has no explicit constructor, so its
// primary-ctor parameter `t` (plain, initializer-position only — §22.10: not
// captured) flows into `_cachedTask`'s initializer, and the NEXT field
// initializer throws. `Boom.Bang()` runs after `_cachedTask := t` has completed,
// so the cleanup block sees a populated borrow slot.
//
// Fails without the fix: `noctor.field.cleanup` ran the full task drop on the
// borrowed `_cachedTask` before propagating, and `t`'s scope-end drop then hung
// forever on the freed task's mutex -> runAotChecked timeout -> -999 != 0.
// (Verified: the same program times out at 20s under a plain ASan probe against
// the pre-fix compiler, and exits 0 clean after.)
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_borrowed_task_field_2260,
           borrowed_task_field_not_disposed_by_throwing_field_init_spec_22_9) {
    // per §22.9 / §22.10 / §28.5
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class Boom "
        "  public static method Bang(): int "
        "    throw new Exception(\"boom\"); "
        "  end Bang; "
        "end Boom; "
        "class Cache(t: Task<int>) "
        "  private _cachedTask: Task<int> := t; "
        "  private _n: int := Boom.Bang(); "
        "  public method Peek(): int => _n; "
        "end Cache; "
        "class App "
        "  public static async method Tick(i: int): Task<int> "
        "    return i; "
        "  end Tick; "
        "  public static method Main(): int "
        "    using var t := App.Tick(1); "
        "    try "
        "      using var c := new Cache(t); "
        "      return 2; "
        "    catch Exception => return 0; "
        "    end; "
        "  end Main; "
        "end App;"), 0);
}

// ---------------------------------------------------------------------------
// #2260 CONTROL — the fix narrows the Task arm to OWNING slots; it must not
// disable it. An `owned` OwningTask<Cell> field is still disposed at holder
// scope end, which disposes the produced-but-unconsumed Cell via the per-T
// result_disposer (§30.5). Cell.n == 0 is the dispose-exactly-once oracle, and
// runAotChecked adds the LSan leg on top.
//
// This is the cell that would fail if the fix were "delete the Task arm":
// without the arm Cell.n stays 1 and LSan reports the task frame.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_borrowed_task_field_2260,
           owned_task_field_still_disposed_control_spec_28_6_30_5) {
    // per §28.6 / §30.5 — the control for the #2260 narrowing
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; " + std::string(kCell) +
        "class Holder "
        "  public owned _task: OwningTask<Cell>; "
        "  public constructor(owned t: OwningTask<Cell>) _task := t; end constructor; "
        "end Holder; "
        "class App "
        "  public static async method Make(): OwningTask<Cell> "
        "    return new Cell(); "
        "  end Make; "
        "  public static method Main(): int "
        "    do using var h := new Holder(App.Make()); end; "
        "    return Cell.n; "
        "  end Main; "
        "end App;"), 0);
}

// ---------------------------------------------------------------------------
// #2260 finding 2 (LEAK) — taskFamilyTypeFromField had no `posParam` arm.
// §22.10 (v0.180): an `owned` primary-constructor parameter referenced in a
// BODY position "is captured as a compiler-generated `owned` private field,
// disposed by the generated `Dispose()` exactly like a declared owned field
// (§28.4)". For such a field the `owned` modifier rides `posParam->modifier`
// and `fs->decl` is the enclosing TypeDecl, so the helper's
// `dynamic_cast<const ast::FieldDecl*>` failed and it returned null — the
// capture slot was never disposed. Every sibling classifier has carried a
// posParam arm since #1594 (LlvmBackend_impl.hpp:5320, :5386, :5434, :5460,
// :5506); the Task arm did not.
//
// The body reference is a BORROW read (`var x := t;`) — enough to trigger the
// §22.10 capture, and not a disposal through the field (which §28.6 forbids).
//
// Fails without the fix: the task frame, its async frame, and the produced
// Cell all leak — Cell.n == 1 and LeakSanitizer reports 208 bytes in 3
// allocations under `bz$App$Make`, so runAotChecked flips to -999. Passes with
// the fix: Cell.n == 0, LSan-clean.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_borrowed_task_field_2260,
           captured_owned_primary_ctor_task_param_disposed_spec_22_10) {
    // per §22.10 (v0.180) / §28.4 / §30.5
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; " + std::string(kCell) +
        "class H(owned t: OwningTask<Cell>) "
        "  public method Keep(): int var x := t; return 1; end Keep; "
        "end H; "
        "class App "
        "  public static async method Make(): OwningTask<Cell> "
        "    return new Cell(); "
        "  end Make; "
        "  public static method Main(): int "
        "    do "
        "      using var h := new H(App.Make()); "
        "      Console.WriteLine(\"keep=\" + h.Keep().ToString()); "
        "    end; "
        "    return Cell.n; "
        "  end Main; "
        "end App;"), 0);
}

// ---------------------------------------------------------------------------
// #2260 finding 2, companion — with the posParam arm in place, the capture
// slot is a full owning Task field, so §28.7 drop-on-overwrite applies to it
// too (the arm feeds isOwnedTaskField / lowerOwnedTaskFieldStore as well as
// the disposal cascade — one predicate, both callers). Reassigning the capture
// disposes the displaced task and its produced Cell exactly once, and the
// replacement is disposed by the cascade: Cell.n == 0 for TWO produced Cells.
//
// Fails without the fix: neither task is disposed (no posParam arm on either
// path) — Cell.n == 2 and LSan reports both frames -> -999.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_borrowed_task_field_2260,
           captured_owned_primary_ctor_task_param_drop_on_overwrite_spec_28_7) {
    // per §22.10 / §28.7 / §30.5
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; " + std::string(kCell) +
        "class H(owned t: OwningTask<Cell>) "
        "  public method Swap(owned u: OwningTask<Cell>) t := u; end Swap; "
        "end H; "
        "class App "
        "  public static async method Make(): OwningTask<Cell> "
        "    return new Cell(); "
        "  end Make; "
        "  public static method Main(): int "
        "    do "
        "      using var h := new H(App.Make()); "
        "      h.Swap(App.Make()); "
        "    end; "
        "    return Cell.n; "
        "  end Main; "
        "end App;"), 0);
}

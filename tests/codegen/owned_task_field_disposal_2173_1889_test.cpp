// Codegen — issues #2173/#1889/#2216 (§28.7 / §30.5 drop-on-overwrite and
// §28.6 scope-end disposal) for an `owned` Task-family field. Without
// the fix, the produced but unconsumed Owned T result of an OwningTask
// <T:class> field's task leaks (the using-var task disposal protocol
// — per-T result_disposer attach + bz_task_fire_and_forget — is wired
// for `using var` locals via #1258/#1868 but not for owned fields).
//
// Spec basis:
//   §28.7 "disposed at the assignment-site before the new value is stored"
//   §30.5 "Disposing an unconsumed OwningTask<T> disposes the result
//          if one was produced"
//   §28.4 "owned fields are disposed in reverse declaration order"
//   §30.10 (checked-mode continuation abandonment diagnostic)
// v0.226.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotCheckedDisposal;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
int runCheckedDisposal(const std::string& prog) {
    auto r = runAotCheckedDisposal(prog);
    return r.ran ? r.exitCode : -999;
}
// Cell: a class with a static counter (ctor ++, dtor --) for leak detection.
const char* kCell =
    "class Cell public static n: int := 0; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; "
    "end Cell; ";
// Data: a small data class with a destructor that prints (so the test
// harness can observe the dispose via stdout if it ever runs).
const char* kData =
    "class Data public _v: int; "
    "  public constructor(v: int) _v := v; end constructor; "
    "  destructor end destructor; "
    "end Data; ";
}  // namespace

// ---------------------------------------------------------------------------
// #1889 — the scope-end case. An `owned _task: OwningTask<Cell>` field in a
// Holder must be disposed when the holder is disposed at scope end. The
// produced-but-unconsumed Cell result is freed via the per-T result_disposer
// — Cell.n must be 0 after the holder goes out of scope.
// Pre-fix behaviour: Cell.n stays 1 (the Cell produced by Produce still
// 'owned' by the task); Task frame / Cell / result slot all leak.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_owned_task_field, owned_task_field_disposed_at_scope_end_1889) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + std::string(kCell) +
        "class Holder "
        "  public owned _task: OwningTask<Cell>; "
        "  public constructor(owned t: OwningTask<Cell>) _task := t; end constructor; "
        "end Holder; "
        "class App "
        "  public static async method Make(i: int): OwningTask<Cell> "
        "    return new Cell(); "
        "  end Make; "
        "  public static async method Main(): Task "
        "    do using var h := new Holder(App.Make(5)); end; "
        "    Console.WriteLine(\"Cell.n=\" + Cell.n.ToString()); "
        "  end Main; "
        "end App;"), 0);
}

// ---------------------------------------------------------------------------
// #1889 — control: a plain `owned _cell: Cell` field in the same holder
// already disposes correctly (the existing class-field disposal path,
// #362/#879). The point of this control is to verify the test harness
// runs the field-disposal code path: a plain owned Cell field reduces
// Cell.n to 0, proving the leakage is specifically the task-family field.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_owned_task_field, plain_owned_cell_field_control_1889) {
    // A plain `owned _cell: Cell` field in the same holder already disposes
    // correctly (the existing class-field disposal path, #362/#879). The
    // point of this control is to verify the test harness runs the field-
    // disposal code path: a plain owned Cell field reduces Cell.n to 0,
    // proving the leakage is specifically the task-family field.
    BEZEL_EXPECT_EQ(run(
        "module M; " + std::string(kCell) +
        "class Holder "
        "  public owned _cell: Cell; "
        "  public constructor(owned c: Cell) _cell := c; end constructor; "
        "end Holder; "
        "class App "
        "  public static method Main(): int "
        "    do using var h := new Holder(new Cell()); end; "
        "    return Cell.n; "
        "  end Main; "
        "end App;"), 0);
}

// ---------------------------------------------------------------------------
// #2173 — the drop-on-overwrite case. After `h._task := App.Make(99)`, the
// OLD task (App.Make(5)) must be disposed, which disposes its produced Cell.
// Cell.n must be 0 after the holder's scope ends (the 99-Cell is also
// produced, then disposed at scope end).
// Pre-fix behaviour: TWO leaks — the 5-Cell from the dropped task, plus
// the 99-Cell from the overwritten task. Cell.n stays at 2.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_owned_task_field, owned_task_field_disposed_on_overwrite_2173) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + std::string(kCell) +
        "class Holder "
        "  public owned _task: OwningTask<Cell>; "
        "  public constructor(owned t: OwningTask<Cell>) _task := t; end constructor; "
        "  public method Set(owned t: OwningTask<Cell>) _task := t; end Set; "
        "end Holder; "
        "class App "
        "  public static async method Make(i: int): OwningTask<Cell> "
        "    return new Cell(); "
        "  end Make; "
        "  public static async method Main(): Task "
        "    do "
        "      using var h := new Holder(App.Make(5)); "
        "      h.Set(App.Make(99)); "
        "    end; "
        "    Console.WriteLine(\"Cell.n=\" + Cell.n.ToString()); "
        "  end Main; "
        "end App;"), 0);
}

// ---------------------------------------------------------------------------
// #2173 — the drop-on-overwrite case for a Task<Data> field (non-OwningTask,
// non-class result). The task struct itself is freed via fire-and-forget's
// continuation; no per-T result_disposer is attached (Data is value-typed,
// the field is Task<Data> not OwningTask<Data>). ASan probe-less (no Cell
// to count) — the test asserts the program exits cleanly without crash.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_owned_task_field, owned_task_field_overwrite_no_result_2173) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + std::string(kData) +
        "class Box "
        "  public owned _t: Task<Data>; "
        "  public constructor(owned t: Task<Data>) _t := t; end constructor; "
        "  public method Swap(owned t: Task<Data>) _t := t; end Swap; "
        "end Box; "
        "class C "
        "  public static async method Produce(v: int): Task<Data> "
        "    return new Data(v); "
        "  end Produce; "
        "  public static async method Main(): Task<int> "
        "    using var box := new Box(C.Produce(7)); "
        "    box.Swap(C.Produce(99)); "
        "    return 0; "
        "  end Main; "
        "end C;"), 0);
}
<<<<<<< HEAD

// ---------------------------------------------------------------------------
// #1889 — collection cascade: an owning collection of Task-family elements
// owns each task frame, and disposing the collection must also dispose an
// unconsumed class result. This is the collection half of the same §30.5
// disposal rule as the field cases above; without a Task-family element
// disposer the task frame and Cell result remain leaked.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_owned_task_collection_1889,
           owning_collection_task_element_disposed_1889) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + std::string(kCell) +
        "class App "
        "  public static async method Make(): OwningTask<Cell> "
        "    return new Cell(); "
        "  end Make; "
        "  public static method Main(): int "
        "    do "
        "      using var tasks := new OwningArray<OwningTask<Cell>>(); "
        "      tasks.Add(App.Make()); "
        "    end; "
        "    return Cell.n; "
        "  end Main; "
        "end App;"), 0);
}

// §30.5 control: Task<T> is the other Task-family element shape. It has no
// consuming result obligation for a value-typed T, but the owning collection
// still owns and must eventually reclaim each task frame.
BEZEL_TEST(codegen_owned_task_collection_1889,
           owning_collection_plain_task_element_disposed_1889) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + std::string(kData) +
        "class App "
        "  public static async method Make(): Task<Data> "
        "    return new Data(1); "
        "  end Make; "
        "  public static method Main(): int "
        "    do "
        "      using var tasks := new OwningArray<Task<Data>>(); "
        "      tasks.Add(App.Make()); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end App;"), 0);
}

// ---------------------------------------------------------------------------
// #2173 — §30.10 backstop: a stale borrow of an owned Task<Data> field's
// produced result, after the field is overwritten by `_t := t` (§28.7
// drop-on-overwrite). The OLD task is disposed (fire-and-forget[_checked]),
// which in checked-disposal mode QUARANTINES the produced Data — its
// disposed flag remains readable so a subsequent member access on the
// stale reference raises DisposedAccessException (§31.4) rather than
// reading freed memory. Pre-fix, the stale use reached into freed heap
// (a UAF under --sanitize=address) or read garbage under release builds
// (the backstop was never installed). The probe shape is the canonical
// critic probe cr2_2042_displacement_backstop (#2042 family); this
// regression test pins the #2173 half — the Task-family field variant
// (the #2043 class-typed backstop was already wired). The test catches
// the exception and returns 1; if the backstop is missing, the access
// either reads garbage (any int, expect 1 fails) or the program crashes
// (no catch — caught by the test harness's failure path). // per
// §28.7, §30.5, §30.10, #2173
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_owned_task_field_2173,
           checked_disposal_backstop_after_overwrite_faults_spec_28_7_30_10) {
    BEZEL_EXPECT_EQ(runCheckedDisposal(
        "module M; " + std::string(kData) +
        "class Box "
        "  public owned _t: Task<Data>; "
        "  public constructor(owned t: Task<Data>) _t := t; end constructor; "
        "  public method Swap(owned t: Task<Data>) _t := t; end Swap; "
        "end Box; "
        "class C "
        "  public static async method Produce(v: int): Task<Data> "
        "    return new Data(v); "
        "  end Produce; "
        "  public static async method Main(): Task<int> "
        "    using var box := new Box(C.Produce(7)); "
        "    var d := await box._t; "         // borrow the old task's result
        "    box.Swap(C.Produce(99)); "       // drop-on-overwrite disposes old
        "    try "
        "      var late := d._v; "            // STALE — must fault (backstop)
        "      return late; "                 // unreachable if backstop fires
        "    catch DisposedAccessException => return 1; "
        "    end; "
        "  end Main; "
        "end C;"), 1);
}
=======
>>>>>>> parent of b6034ff (fix #1889: dispose Task-family elements in owning collections)

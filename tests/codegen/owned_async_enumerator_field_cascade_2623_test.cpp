// Codegen — issue #2623 (leak): an `owned` field holding an ASYNC ITERATOR
// (IAsyncEnumerator<T>) is not disposed when its owner is.
//
// Spec basis (v0.345):
//   §28.5  the owned-field cascade: disposing the owner disposes its `owned`
//          fields, in reverse declaration order.
//   §29.7  the disposal of a partially-consumed iterator: the frame's Dispose
//          runs the pending `finally` chain at the suspension state and frees
//          the frame itself.
//   §30.7  the async iterator frame's vtable is
//          [0]=td, [1]=Dispose, [2]=MoveNextAsync, [3]=Current,
//          [4]=GetAsyncEnumerator — the SAME slot-1 Dispose the synchronous
//          iterator format has (the async frame carries no `DisposeAsync`:
//          §30.7 says the synchronous fallback (§29.7) runs for an
//          iterator-format `await using`).
//
// THE DEFECT. `emitOwnedFieldDisposals` classified an `owned` field through
// `isOwnedIteratorInterfaceField`, which recognised the SYNCHRONOUS
// iterator-format heads only (IEnumerable<T> / IOwningEnumerable<T>). An
// `owned IAsyncEnumerator<T>` field matched no arm at all, so the owner's
// disposal never touched it: the async iterator FRAME leaked (72 bytes,
// allocated in `bz$asynciter$...$GetAsyncEnumerator`) AND the §29.7 `finally`
// chain at the suspension point never ran — the issue's direct evidence that
// the field's Dispose never ran.
//
// THE FIX. The owned-field classification admits the ASYNC iterator-format
// heads as well (the two enumerator heads `common::isIterFormatEnumeratorHead`
// holds beyond its synchronous half), and the SAME slot-1 dispatch frees the
// frame. The dispatch chooses between the iterator arm and the class arm at
// RUN TIME (the discriminator answers "is word 0 a REGISTERED vtable?"), so a
// user class implementing IAsyncEnumerator<T> (§8.1) still takes the class arm
// and the frame takes the slot-1 arm.
//
// WHAT IS NOT PINNED HERE. The issue's own repro puts
// `En!.Dispose()` INSIDE the iterator's own `finally` — a disposal of the
// frame from within the frame's disposal. That is a SEPARATE defect (it
// reproduces on the SYNCHRONOUS twin today, on a tree without this fix, and is
// reported to the coordinator), so this file pins the two symptoms the issue
// names — the frame is freed, and the `finally` chain runs — on a program that
// isolates them, and pins the `finally`-runs symptom on the issue's own
// program under the non-sanitizer oracle.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — the AOT oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// The strict oracle: AOT + ASan/LSan. A sanitizer report, a crash, or a run
// that never terminates all surface as ran=false -> -999.
int runChecked(const std::string& src) {
    JitResult r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// The plain AOT oracle, with the captured stdout (for the `finally`-runs
// observable, which cannot round-trip a process exit status).
JitResult runPlain(const std::string& src) {
    return runAot(src);
}

// The class under test: an `owned` field holding an async enumerator, and an
// async iterator method that yields twice and has a `finally` block.
const char* kHdr =
    "module M; "
    "class H "
    "  public owned En: IAsyncEnumerator<int>? := null; "
    "  public async method Gen(): IAsyncEnumerable<int> "
    "    try "
    "      yield return 1; "
    "      yield return 2; "
    "    finally "
    "      Console.WriteLine(\"fin\"); "
    "    end; "
    "  end Gen; "
    "end H; ";

}  // namespace

// ---------------------------------------------------------------------------
// #2623 — the field's cascade. The owner's scope-end disposal must dispose
// the `owned` async-enumerator field, which frees the async iterator FRAME
// (the 72-byte allocation in `bz$asynciter$M.H$Gen$GetAsyncEnumerator`).
//
// Fails without the fix: the owner is disposed, the field is not, and the
// frame leaks -> LSan reports "72 byte(s) leaked in 1 allocation(s)" ->
// runAotChecked reports ran=false -> -999 != 1.
//
// The program's own exit code is 1 (the sum of the first yielded value), and
// `fin` is printed by the finally chain the field's Dispose now runs.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_owned_async_enumerator_field_2623,
           owned_async_enumerator_field_cascade_frees_the_frame_spec_28_5_29_7) {
    // per §28.5 / §29.7 / §30.7 (v0.345)
    BEZEL_EXPECT_EQ(runChecked(
        std::string(kHdr) +
        "class C "
        "  public static async method Main(): Task<int> "
        "    var sum := 0; "
        "    using var h := new H(); "
        "    h.En := h.Gen().GetAsyncEnumerator(); "
        "    var m := await h.En!.MoveNextAsync(); "
        "    if m then sum := sum + h.En!.Current; end; "
        "    Console.WriteLine(\"sum=\" + sum.ToString()); "
        "    return sum; "
        "  end Main; "
        "end C;"), 1);
}

// ---------------------------------------------------------------------------
// #2623 — the SECOND symptom the issue names: "the §29.7 `finally` chain is
// skipped". The issue's OWN program (its `finally` also disposes the
// enumerator, which is a separate defect — reported to the coordinator, and
// reproducible on the synchronous twin without this fix) is pinned here under
// the non-sanitizer oracle: the `finally` chain must run, which is observable
// as `fin` on stdout.
//
// Fails without the fix: the field's Dispose never runs, so the `finally`
// never runs, and `fin` is absent from stdout (the program still exits 1).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_owned_async_enumerator_field_2623,
           owned_async_enumerator_field_cascade_runs_the_finally_chain_spec_29_7) {
    JitResult r = runPlain(
        "module M; "
        "class H "
        "  public owned En: IAsyncEnumerator<int>? := null; "
        "  public async method Gen(): IAsyncEnumerable<int> "
        "    try "
        "      yield return 1; "
        "      yield return 2; "
        "    finally "
        "      Console.WriteLine(\"fin\"); "
        "      if En <> null then En!.Dispose(); end; "
        "    end; "
        "  end Gen; "
        "end H; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var sum := 0; "
        "    using var h := new H(); "
        "    h.En := h.Gen().GetAsyncEnumerator(); "
        "    var m := await h.En!.MoveNextAsync(); "
        "    if m then sum := sum + h.En!.Current; end; "
        "    Console.WriteLine(\"sum=\" + sum.ToString()); "
        "    return sum; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("fin") != std::string::npos);
}

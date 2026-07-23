// Codegen/runtime — issue #2132 (§30.2 async frame spill / §28.9 / §28.11,
// borrowing-collection reclaim): a heap collection built from a `[...]` LITERAL
// and held live ACROSS an `await` suspend point in an async body is corrupted on
// resume — reading it faults (a garbage frame slot → SEGV in bz_array_len), and
// even a use-only-before-await case leaks the container + backing (the scope-end
// drop on the resume path frees a garbage / never-freed reference).
//
// Two independent defects, both surfaced by a collection LITERAL local:
//
//   1. FRAME SPILL (the SEGV / wrong-value on read). The general lowerLocalDecl
//      slot path maps a suspend-live local's alloca to its reserved async-frame
//      field via smAllocaToField_, so emitAwaitSuspend spills it before the
//      suspend and restores it on the (fresh-stack) resume. The collection-literal
//      / dict-literal / inline-array EARLY-RETURN initializer paths built the slot
//      but bypassed that registration — so a `var xs: Array<T> := [ … ]` held
//      across an await was NEVER spilled/restored and held garbage on resume.
//      Fixed by mapSuspendLocalSlot at those early-return sites (Setup.cpp).
//
//   2. RECLAIM (the drop-after-await leak). A plain `var` borrowing-`Array<T>`
//      literal's backing is "reclaimed with the instance" (§28.9/§28.11) via a
//      compiler-emitted scope-end bz_array_free, registered when
//      `currentFn_ == borrowingArrayFreeEligibleFn_`. The async body lowers in the
//      RESUME function while emitFunctionBody pinned that key to the ENTRY function
//      — so the reclaim was silently skipped and the backing leaked on the async
//      return edge. Fixed by re-establishing the borrowing-array (and its
//      co-computed dict/grouping) eligibility keyed to the resume fn in BOTH the
//      async-method and async-lambda resume setups (LlvmBackend_Async.cpp),
//      mirroring the closure / unwind / cell re-establishment already there.
//
// OwningArray<T> via `new` was the issue's CONTRAST (unaffected): it lowers via
// the general slot path (spilled) and its `using var` disposal is re-established
// for the resume fn already — so it stayed correct throughout. It is kept here as
// a green control.
//
// fail-without/pass-with. Without the fix each read case SEGVs (runAotChecked ->
// ran=false) and each drop case leaks (LSan -> ran=false); the value cases return
// the wrong exit code. With the fix every program is ASan/LSan-clean and returns
// the expected value. // per §30.2, §28.9, §28.11 (issue #2132)

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
// runAotChecked returns ran=false on any sanitizer fire (SEGV read / leak drop);
// map that to a sentinel so a single EXPECT_EQ pins BOTH memory-cleanliness AND
// the computed value.
int runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}

// A Box with a public int field — a trivial-dispose class element admissible in
// an OwningArray<Box> literal (§15.4: a fresh producer of trivial-dispose class
// type is well-formed).
const char* kBox =
    "class Box public v: int := 0; "
    "  public constructor(x: int) v := x; end constructor; end Box; ";
}  // namespace

// ── Repro A: SEGV on READ after resume. `var tasks: Array<int> := [ … ]` read
// after an await. Without the frame-spill fix the frame slot holds garbage on
// resume and `tasks[0]`/`tasks.Count` fault in bz_array_len. Value: 10 + 3 = 13.
// per §30.2 (issue #2132)
BEZEL_TEST(async_array_frame_spill_2132, borrowing_array_literal_read_after_await) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var tasks: Array<int> := [10, 20, 30]; "
        "    await Task.Delay(30); "
        "    return tasks[0] + tasks.Count; "   // SEGV here without the fix
        "  end Main; "
        "end C;"),
        13);
}

// ── Repro B: LEAK when the array is used only BEFORE the await. The scope-end
// auto-reclaim on the resume path must free the literal's backing exactly once.
// Without the reclaim fix the async return edge skips the free → 104 B leak.
// Value: pre = 10 + 3 = 13. // per §28.9, §28.11 (issue #2132)
BEZEL_TEST(async_array_frame_spill_2132, borrowing_array_literal_drop_after_await_leak_free) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var tasks: Array<int> := [10, 20, 30]; "
        "    var pre := tasks[0] + tasks.Count; "  // read fine before suspend
        "    await Task.Delay(30); "
        "    return pre; "                          // array dropped on resume path
        "  end Main; "
        "end C;"),
        13);
}

// ── The shared frame-spill defect is not borrowing-specific: an OwningArray<Box>
// built from a LITERAL (rather than `new` + Add) took the same early-return path
// and SEGV'd on read after resume. Its `using var` disposal keeps it leak-free
// once the slot round-trips. Value: a[0].v + a.Count = 10 + 3 = 13.
// per §30.2 (issue #2132)
BEZEL_TEST(async_array_frame_spill_2132, owning_array_literal_read_after_await) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; " + std::string(kBox) +
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var a: OwningArray<Box> := [new Box(10), new Box(20), new Box(30)]; "
        "    await Task.Delay(30); "
        "    return a[0].v + a.Count; "
        "  end Main; "
        "end C;"),
        13);
}

// ── Nested: an Array<T> literal declared inside an async LAMBDA body and held
// across the lambda's own await. The lambda drives a SEPARATE state machine
// (buildAsyncLambda) whose resume setup also has to re-establish the reclaim
// eligibility — without it the literal was corrupted on resume (IndexOutOfRange /
// SEGV) and leaked. Value: 10 + 3 = 13. // per §30.2, §28.11 (issue #2132)
BEZEL_TEST(async_array_frame_spill_2132, borrowing_array_literal_in_async_lambda) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var f := async lambda(): Task<int> => do "
        "      var tasks: Array<int> := [10, 20, 30]; "
        "      await Task.Delay(30); "
        "      return tasks[0] + tasks.Count; "
        "    end; "
        "    return await f(); "
        "  end Main; "
        "end C;"),
        13);
}

// ── An Array<int> literal passed as a borrowing argument across the await, then
// read inside the callee. The literal in Main is proven non-escaping (a borrow to
// the callee), so it is reclaimed at Main's scope end on the resume path; the
// callee's borrowed param `xs` (a frame CAPTURE, a separate mechanism) reads
// correctly after ITS await. Value: xs[0] + xs.Count = 10 + 3 = 13.
// per §30.2, §28.11 (issue #2132)
BEZEL_TEST(async_array_frame_spill_2132, borrowing_array_literal_passed_as_borrow_arg) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static async method Use(xs: Array<int>): Task<int> "
        "    await Task.Delay(30); "
        "    return xs[0] + xs.Count; "
        "  end Use; "
        "  public static async method Main(): Task<int> "
        "    var arr: Array<int> := [10, 20, 30]; "
        "    return await C.Use(arr); "
        "  end Main; "
        "end C;"),
        13);
}

// ── STRING-ELEMENT axis: a `var parts := s.Split(...)` — a fresh borrowing
// `Array<string>` MATERIALIZER (isFreshBorrowingArrayMaterializerLocal) whose
// scope-end reclaim carries a NON-null per-element disposer (the string-release
// thunk), unlike the int/class-borrow literal's null disposer. Its reclaim
// eligibility must be re-established for the resume fn too, or the backing AND
// every substring leak on the async return edge. Distinct from the int payload —
// exercises the string-disposer arm of buildCollectionTempDisposal/elementPolicy.
// Value: parts.Count = 3. // per §28.9, §28.11 / #1027 (issue #2132)
BEZEL_TEST(async_array_frame_spill_2132, borrowing_array_string_materializer_across_await) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var parts := \"a,b,c\".Split(\",\"); "
        "    await Task.Delay(30); "
        "    return parts.Count; "
        "  end Main; "
        "end C;"),
        3);
}

// ── Nested-lambda ordering guard: an Array<int> literal in the enclosing async
// body that FOLLOWS a nested async lambda. emitAsyncLambda recomputes the
// borrowing-array reclaim eligibility and keys it to the LAMBDA's resume fn;
// lowerLambda must save/restore that eligibility so this later literal is still
// registered against the ENCLOSING method's resume fn — otherwise its reclaim is
// registered under the stale lambda key and skipped (leak). Value:
// ys[0] + ys.Count + r = 10 + 3 + 1 = 14. // per §30.2, §28.11 (issue #2132)
BEZEL_TEST(async_array_frame_spill_2132, borrowing_array_literal_after_nested_async_lambda) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var f := async lambda(): Task<int> => do "
        "      await Task.Delay(20); "
        "      return 1; "
        "    end; "
        "    var r := await f(); "
        "    var ys: Array<int> := [10, 20, 30]; "   // literal AFTER the lambda
        "    await Task.Delay(20); "
        "    return ys[0] + ys.Count + r; "
        "  end Main; "
        "end C;"),
        14);
}

// ── CONTROL (the issue's contrast, must stay green): OwningArray<Box> built with
// `new` + Add across the same suspend. It lowers via the general slot path and its
// `using var` disposal is already resume-fn-aware — correct before AND after this
// fix. Guards against a regression in the untouched owning path. Value:
// boxes[0].v + boxes.Count = 7 + 1 = 8. // per §30.2 (issue #2132 contrast)
BEZEL_TEST(async_array_frame_spill_2132, owning_array_via_new_across_await_control) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; " + std::string(kBox) +
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var boxes: OwningArray<Box> := new OwningArray<Box>(); "
        "    boxes.Add(new Box(7)); "
        "    await Task.Delay(30); "
        "    return boxes[0].v + boxes.Count; "
        "  end Main; "
        "end C;"),
        8);
}

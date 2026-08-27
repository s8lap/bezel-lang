// Codegen — issue #3437 (§28.12 asynchronous disposal, §29.1 the iteration
// protocol, §29.7 disposal of partially-consumed iterators, §29.11 multiple
// enumeration, §29.2 desugaring of `for`, spec v0.283): an `await using var`
// bound at one of the six ITERATOR-FORMAT interface heads never reached the
// iterator-format disposal arm, because that arm was gated `!ld->isAwaitUsing`.
// The SPELLING decided the disposal; the REPRESENTATION did not.
//
// The six heads — IEnumerable / IOwningEnumerable / IEnumerator /
// IOwningEnumerator / IAsyncEnumerable / IOwningAsyncEnumerable — all name the
// iterator-format frame, vtable [0]=td, [1]=Dispose, [2]=MoveNext, [3]=Current.
// None of them is `IAsyncDisposable`: §28.12 defines asynchronous cleanup as the
// awaited `DisposeAsync()`, and only the two ENUMERATOR names
// (IAsyncEnumerator / IOwningAsyncEnumerator) carry it — they are not in this
// set and keep their own arm. So for these heads there is nothing to await, and
// the `await` is a no-op over the same synchronous slot-1 Dispose the plain
// `using var` gets. That is #2739's ruling for the builtin collections ("an
// `await using var` over a builtin collection has no user-async-disposable
// shape (no DisposeAsync to await) so the `await` is a no-op — apply the same
// synchronous collection disposal"), applied to the family it left out.
//
// TWO DISTINCT MISCOMPILES, both measured on 1e3861a with
// `bezelc --sanitize=address` (LeakSanitizer on), each against its own
// plain-`using var` twin — the twins differ from the repro by the keyword alone:
//
//  1. THE COLLECTION ARM, over a frame. Skipping the iterator arm dropped the
//     decl into the `isAwaitUsing` branch's collection arms, whose
//     `collectionTypeOfLocal` answers from the INITIALIZER's type (`Array<int>`)
//     while the slot holds the §29.2 wrapper frame `lowerLocalDecl`'s coercion
//     minted. `bz_array_free` then read the frame's `{vtable, arr, idx, esz}`
//     as a `bz_array_t` and freed its word 1:
//
//       await using var e: IEnumerable<int> := a;              (named Array)
//       await using var e: IEnumerable<int> := new Array<int>(2, 7);
//         → ASan: "SEGV on unknown address 0x000000589a50 ... #2 bz_array_free
//           ... #3 bz$async$Run$Resume" — a WRITE fault inside the allocator on
//           a non-heap (global vtable) address.
//       using var e: IEnumerable<int> := a;   (twin)  → 14, clean.
//
//  2. MoveNext AS Dispose. When no collection predicate matched either, the
//     decl fell all the way through to the generic `asyncDispose` fallback with
//     a null `disposeAsyncFn` (`classOfLocal` cannot answer through a
//     declaration-less head), which `emitAsyncDisposeAwaitAndFree` degrades to
//     `emitDisposeAndReclaim` → `emitDynamicDispose`: load word 0 as a CLASS
//     vtable, call slot 2. In the iterator format slot 2 is MoveNext. The frame
//     was therefore RESUMED at scope end and then freed with its own Dispose —
//     and every `using var` pending inside its state machine — never run:
//
//       await using var en := Gen();   (IEnumerator<int> yield-iterator holding
//                                       `using var buf := new Array<int>(256,1)`,
//                                       abandoned after one MoveNext, x500)
//         → LSan: 540000 byte(s) leaked in 1000 allocation(s).
//       using var en := Gen();   (twin)  → 249, clean.
//
// This is #2975's wild-jump shape one step short of a garbage function pointer,
// and #759 records the identical word-0 read for the async enumerator names.
//
// THE FIX is one gate: the iterator-format arm in `registerUsingDisposal`
// (LlvmBackend_Setup.cpp) no longer excludes `await using`, so all six heads get
// the slot-1 Dispose in both spellings. A second hunk adds a representation
// GUARD on the generic async fallback — if `iterFormatWrappedDecls_` (the wrap
// creator's own #2975 self-report, the same authority #3384 and #3418 read; no
// new predicate) says this slot holds a minted frame, emit a loud `internal:`
// note and register nothing, so the frame LEAKS rather than being read as a
// class vtable. That is the recoverable direction, and the trade #2992's
// end-of-chain guard and #3157's vtable-less arm already record. It is
// unreachable as of the first hunk: the coercion only records a decl whose
// declared head is `IEnumerable`/`IOwningEnumerable`, and the arm reads
// `ld->type` first.
//
// Note a null `classOfLocal` is NOT the discriminator and deliberately is not
// used as one — `await using var d: IAsyncDisposable := Make();` has none
// either, and there the slot really does hold a class object for which
// `emitDisposeAndReclaim` is right.
//
// FAIL-WITHOUT / PASS-WITH, program by program, one build of each compiler
// (differing only by this fix's hunks):
//
//   program                                            pre-fix        post-fix
//   await using, IEnumerable head, named Array         SEGV           14
//   using var,   IEnumerable head, named Array (ctrl)  14             14
//   await using, IEnumerable head, new Array           SEGV           14
//   using var,   IEnumerable head, new Array (ctrl)    14             14
//   await using, IEnumerable head, OwningArray x50     SEGV           199
//   using var,   IEnumerable head, OwningArray (ctrl)  199            199
//   await using, IOwningEnumerable head, x50           SEGV           199
//   await using, IEnumerator yield-iter, abandoned     540000 B/1000  249
//   using var,   IEnumerator yield-iter (ctrl)         249            249
//   await using, IEnumerable head, yield-iter source   7              7
//   await using, IAsyncEnumerable head, x200           253000 B/800   149
//   await using, Array<int> head (#2739 parity)        165            165
//
// Every SEGV row is the same fault — `bz_array_free`
// (bezel_runtime_collections.cpp) over a wrapper frame; the two leak rows are
// LeakSanitizer on the un-run slot-1 Dispose. Run as one filter on one build of
// each compiler: 6 of 12 fail without the fix (every `await using` row except
// the two the arm already reached by another route), 12 of 12 pass with it, and
// all six that pass without it are controls or parity rows — no row of this file
// can pass for a reason other than the one it names.
//
// Every test runs under runAotChecked (generated-code ASan with LSan), so
// `r.ran` is itself the memory-safety assertion, and each returns a distinctive
// value through `Main` so a silent mis-execution cannot pass.
// per §28.12, §29.1, §29.2, §29.7, §29.11 (issue #3437; #2739, #2975, #3418)

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
// The program's exit code, or a distinctive sentinel when the checked run did
// not complete cleanly (a crash, a leak, a double free) so a failure diff names
// the oracle rather than a bare 0.
int runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}

// An `Array<int>` of two 7s reached through a local declared at `head`, summed.
// `kw` is `await using var` or `using var`; `src` is the initializer. Returns
// 14. The named-source form declares the array as its own `using var` first, so
// the wrap BORROWS it and the §28.10 borrow-pin (lifted by #3418) is in play;
// the `new` form gives the wrap sole ownership and needs no #3418 involvement.
std::string sumTwo(const std::string& kw, const std::string& head,
                   const std::string& pre, const std::string& src) {
    return
        "module M; "
        "class C "
        "  public static async method Run(): Task<int> "
        "    " + pre +
        "    " + kw + " e: " + head + "<int> := " + src + "; "
        "    var n := 0; "
        "    for var v in e do n := n + v; end; "
        "    return n; "
        "  end Run; "
        "  public static method Main(): int "
        "    return C.Run().Result; "
        "  end Main; "
        "end C;";
}

// A named `OwningArray<Item>` reached through a local declared at `head`, fifty
// times. Each execution mints one wrapper frame; 50 * 9 = 450, 450 mod 251 =
// 199. The class-element source is the shape #3418's own grid opens with.
std::string owningArrayLoop(const std::string& kw, const std::string& head) {
    return
        "module M; "
        "class Item "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Item; "
        "class C "
        "  public static async method Run(): Task<int> "
        "    using var a := new OwningArray<Item>(); "
        "    a.Add(new Item(4)); "
        "    a.Add(new Item(5)); "
        "    " + kw + " e: " + head + "<Item> := a; "
        "    var n := 0; "
        "    for var it in e do n := n + it.V; end; "
        "    return n; "
        "  end Run; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 1..50 do t := t + C.Run().Result; end; "
        "    return t mod 251; "
        "  end Main; "
        "end C;";
}

// An `IEnumerator<int>` yield-iterator holding a 256-int `using var` buffer,
// ABANDONED after a single MoveNext, five hundred times. Disposal of `en` must
// take the frame's slot-1 Dispose, which drives the state machine to its
// terminal state and runs that pending `using var` (§29.7); calling slot 2
// (MoveNext) and freeing the frame instead strands one buffer per execution.
// 500 * 1 = 500, 500 mod 251 = 249.
std::string abandonedIterator(const std::string& kw) {
    return
        "module M; "
        "class C "
        "  public static method Gen(): IEnumerator<int> "
        "    using var buf := new Array<int>(256, 1); "
        "    yield return buf[0]; "
        "    yield return buf[1]; "
        "    yield return buf[2]; "
        "  end Gen; "
        "  public static async method Run(): Task<int> "
        "    " + kw + " en := C.Gen(); "
        "    var n := 0; "
        "    if en.MoveNext() then n := en.Current; end; "
        "    return n; "
        "  end Run; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 1..500 do t := t + C.Run().Result; end; "
        "    return t mod 251; "
        "  end Main; "
        "end C;";
}
}  // namespace

// ── REPRO 1a: the `IEnumerable<T>` head over a NAMED `Array<int>`. The §29.2
// coercion mints a borrowing wrapper frame; #3418 lifts the §28.10 borrow-pin
// so the decl reaches the registration at all, and pre-fix it then landed in the
// collection arm and ran `bz_array_free` over that frame (ASan SEGV).
// per §28.12, §29.2, §28.10 (issue #3437, #3418)
BEZEL_TEST(await_using_iterator_format_disposal_3437,
           await_using_ienumerable_head_named_array_spec_28_12_29_2) {
    BEZEL_EXPECT_EQ(runChecked(sumTwo(
        "await using var", "IEnumerable",
        "using var a := new Array<int>(2, 7); ", "a")), 14);
}

// ── PARITY CONTROL: the same program with the plain keyword, clean before and
// after — the keyword is the only difference. // per §28.10 (issue #3437)
BEZEL_TEST(await_using_iterator_format_disposal_3437,
           using_var_ienumerable_head_named_array_control_spec_28_10) {
    BEZEL_EXPECT_EQ(runChecked(sumTwo(
        "using var", "IEnumerable",
        "using var a := new Array<int>(2, 7); ", "a")), 14);
}

// ── REPRO 1b: the twin the issue notes needs NO #3418 involvement — the wrap
// owns its source, so no borrow-pin is ever established. This is what makes the
// defect class strictly older than #3418. // per §28.12, §29.2 (issue #3437)
BEZEL_TEST(await_using_iterator_format_disposal_3437,
           await_using_ienumerable_head_new_array_spec_28_12_29_2) {
    BEZEL_EXPECT_EQ(runChecked(sumTwo(
        "await using var", "IEnumerable", "", "new Array<int>(2, 7)")), 14);
}

BEZEL_TEST(await_using_iterator_format_disposal_3437,
           using_var_ienumerable_head_new_array_control_spec_28_10) {
    BEZEL_EXPECT_EQ(runChecked(sumTwo(
        "using var", "IEnumerable", "", "new Array<int>(2, 7)")), 14);
}

// ── SOURCE-FAMILY axis: a class-element `OwningArray<Item>`, fifty executions,
// so a per-execution frame miscount is visible as a leak as well as a crash.
// per §28.12, §29.2, stdlib §4.5 (issue #3437)
BEZEL_TEST(await_using_iterator_format_disposal_3437,
           await_using_ienumerable_head_owning_array_spec_28_12_29_2) {
    BEZEL_EXPECT_EQ(runChecked(owningArrayLoop("await using var",
                                               "IEnumerable")), 199);
}

BEZEL_TEST(await_using_iterator_format_disposal_3437,
           using_var_ienumerable_head_owning_array_control_spec_28_10) {
    BEZEL_EXPECT_EQ(runChecked(owningArrayLoop("using var", "IEnumerable")),
                    199);
}

// ── THE SECOND WRAPPED HEAD: `IOwningEnumerable<T>` mints the DRAINING frame
// instead (§29.1's `TakeCurrent` protocol), whose Dispose also reclaims an
// advanced-but-untaken pending. Both heads are what `lowerLocalDecl`'s coercion
// gate admits, so both must reach the same arm.
// per §28.12, §29.1, §29.2 (issue #3437)
BEZEL_TEST(await_using_iterator_format_disposal_3437,
           await_using_iowningenumerable_head_owning_array_spec_29_1) {
    BEZEL_EXPECT_EQ(runChecked(owningArrayLoop("await using var",
                                               "IOwningEnumerable")), 199);
}

// ── REPRO 2: MoveNext-as-Dispose. No collection predicate matches an
// `IEnumerator<int>` yield-iterator, so this shape fell through to the generic
// async fallback and its word-0 read — resuming the frame and freeing it with
// the pending `using var` inside its state machine never discharged.
// 540000 B / 1000 allocations pre-fix. // per §28.12, §29.7, §29.3 (issue #3437)
BEZEL_TEST(await_using_iterator_format_disposal_3437,
           await_using_ienumerator_abandoned_iterator_spec_29_7_28_12) {
    BEZEL_EXPECT_EQ(runChecked(abandonedIterator("await using var")), 249);
}

// ── PARITY CONTROL for repro 2: the plain keyword takes the slot-1 Dispose and
// was clean before and after. // per §29.7, §28.10 (issue #3437)
BEZEL_TEST(await_using_iterator_format_disposal_3437,
           using_var_ienumerator_abandoned_iterator_control_spec_29_7) {
    BEZEL_EXPECT_EQ(runChecked(abandonedIterator("using var")), 249);
}

// ── AN UNWRAPPED ITERATOR-FORMAT SOURCE: the initializer's static type is
// already `IEnumerable<int>` (an iterator method's own return), so the §29.2
// coercion declines and the slot holds the state machine directly. The head
// still names the iterator format, so the same arm must claim it. Clean before
// the fix too — a parity row that pins the arm does not over-claim.
// per §28.12, §29.3, §29.11 (issue #3437)
BEZEL_TEST(await_using_iterator_format_disposal_3437,
           await_using_ienumerable_head_iterator_source_spec_29_3_29_11) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static method Gen(): IEnumerable<int> "
        "    yield return 3; "
        "    yield return 4; "
        "  end Gen; "
        "  public static async method Run(): Task<int> "
        "    await using var e: IEnumerable<int> := C.Gen(); "
        "    var n := 0; "
        "    for var v in e do n := n + v; end; "
        "    return n; "
        "  end Run; "
        "  public static method Main(): int "
        "    return C.Run().Result; "
        "  end Main; "
        "end C;"), 7);
}

// ── THE ASYNC ENUMERABLE HEAD. `IAsyncEnumerable<T>` does not itself extend
// `IAsyncDisposable` (#1525 records this: only the ENUMERATOR names do), so its
// disposal is synchronous too and it belongs in the same arm. 200 executions.
// 200 * 2 = 400, 400 mod 251 = 149. // per §28.12, §30.7, §29.13 (issue #3437)
BEZEL_TEST(await_using_iterator_format_disposal_3437,
           await_using_iasyncenumerable_head_spec_28_12_30_7) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    using var buf := new Array<int>(256, 1); "
        "    yield return buf[0]; "
        "    yield return buf[1]; "
        "  end Gen; "
        "  public static async method Run(): Task<int> "
        "    await using var src: IAsyncEnumerable<int> := C.Gen(); "
        "    var n := 0; "
        "    await for var v in src do n := n + v; end; "
        "    return n; "
        "  end Run; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 1..200 do t := t + C.Run().Result; end; "
        "    return t mod 251; "
        "  end Main; "
        "end C;"), 149);
}

// ── #2739 PARITY: an `await using var` at a BUILTIN COLLECTION head must still
// take the collection arm. The iterator-format arm now runs BEFORE it for both
// spellings, so this pins that the reordering did not capture the collection
// heads. 3 * (1 + ... + 10) = 165, the value #2739's own test asserts.
// per §30.5, §30.6, §28.11 (issue #3437, #2739)
BEZEL_TEST(await_using_iterator_format_disposal_3437,
           await_using_array_head_collection_arm_parity_2739_spec_28_11) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class C "
        "  public static async method Sum(k: int): Task<int> "
        "    await using var xs: Array<int> := [k, k, k]; "
        "    var n := 0; "
        "    for var v in xs do n := n + v; end; "
        "    return n; "
        "  end Sum; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    for var i in 1..10 do total := total + C.Sum(i).Result; end; "
        "    return total; "
        "  end Main; "
        "end C;"), 165);
}

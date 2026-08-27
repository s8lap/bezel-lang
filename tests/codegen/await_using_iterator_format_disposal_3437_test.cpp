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
//
// ⚠ NOT because none of them is `IAsyncDisposable`. That claim was made in an
// earlier revision of this header and it is FALSE: stdlib §4.13 (v0.283)
// declares `public interface IOwningAsyncEnumerable<T>: IAsyncDisposable where
// T: class` (Bezel_Standard_Library.md:2441), so THREE iteration interfaces
// carry `IAsyncDisposable` — IAsyncEnumerator, IOwningAsyncEnumerator and
// IOwningAsyncEnumerable — and the third of them IS in this set.
//
// The justification that holds is §28.12's own synchronous-fallback clause:
// asynchronous cleanup "runs only where the program wrote `await using` (whose
// implicit `finally` awaits `DisposeAsync` on both the normal and exception
// edges of an async body, with §29.7's synchronous fallback on early iterator
// abandonment)" (§28.12 verbatim, v0.283), with §29.7 the matching half —
// "When a consumer stops iterating early — `break`, exception, or `using var`
// ending before exhaustion — the iterator is disposed", running the pending
// cleanups "including, via §28.12's synchronous-fallback path, `await using
// var` disposals" (§29.7 verbatim, v0.283). A synchronous disposal of an
// iterator frame is the spec's DEFINED degraded behaviour, not a liberty.
//
// And it is the path that ran either way. The value at any of these heads is an
// iterator-format frame, so no `DisposeAsync` pointer is reachable through it:
// `classOfLocal` cannot answer for a declaration-less builtin interface head,
// `disposeAsyncFn` stays null, and `emitAsyncDisposeAwaitAndFree` degrades to
// the synchronous `emitDisposeAndReclaim` by its own documented contract. So
// the fix trades no awaited `DisposeAsync` for a synchronous one — none was
// ever emitted at these heads. It changes only WHICH synchronous disposal runs:
// the frame's own slot-1 Dispose, instead of word 0 read as a class vtable.
// That is #2739's disposition for the builtin collections ("an `await using
// var` over a builtin collection has no user-async-disposable shape (no
// DisposeAsync to await) so the `await` is a no-op — apply the same synchronous
// collection disposal"), reached here by §28.12/§29.7.
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
//     THE RESUMPTION IS THE HEADLINE, not the free. Calling slot 2 does not
//     merely fail to dispose — it DRIVES THE STATE MACHINE, so the scope-end
//     "disposal" silently executes user statements from the iterator body that
//     the program never asked to run. Measured with a generator that increments
//     a borrowed counter cell between yields, driven by exactly ONE explicit
//     `MoveNext`: the cell reads 2 under `await using` and 1 under the
//     `using var` twin (the probe returns 210 vs 110; both 110 with the fix).
//     A wild free is a memory fault; this is unrequested execution of user code
//     ahead of it, and it is the strongest reason the gate had to move.
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
//   await using, IOwningEnumerator head, x300          328800 B/900   49
//   using var,   IOwningEnumerator head (ctrl)         49             49
//   await using, IOwningAsyncEnumerable head, x300     384300 B/1500  98
//   using var,   IOwningAsyncEnumerable head (ctrl)    98             98
//   await using, IEnumerator RESUMPTION detector       210            110
//   using var,   IEnumerator RESUMPTION (ctrl)         110            110
//   await using, IEnumerable head, yield-iter source   7              7
//   await using, IAsyncEnumerable head, x200           253000 B/800   149
//   await using, Array<int> head (#2739 parity)        165            165
//
// Every SEGV row is the same fault — `bz_array_free`
// (bezel_runtime_collections.cpp) over a wrapper frame; the leak rows are
// LeakSanitizer on the un-run slot-1 Dispose. Run as one filter on one build of
// each compiler: 9 of 18 fail without the fix, 18 of 18 pass with it. The 9 that
// pass without it are the 7 plain-`using var` controls plus two `await using`
// rows the arm already reached by another route (the unwrapped yield-iterator
// source, and the #2739 `Array<int>`-head parity row) — so no row of this file
// can pass for a reason other than the one it names.
//
// ALL SIX heads the arm claims now have a row: IEnumerable (three sources),
// IOwningEnumerable, IEnumerator (two shapes), IOwningEnumerator,
// IAsyncEnumerable, IOwningAsyncEnumerable. An earlier revision of this file
// covered four of the six, which would have let a green gate vindicate the arm
// without ever exercising two of its names — both of which were in fact broken.
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

// An `IOwningEnumerator<Item>` yield-iterator (§29.3's single-use owning form,
// stdlib §4.13 `IOwningEnumerator<T>: IDisposable where T: class`) holding a
// 256-int `using var` buffer, ABANDONED after one `MoveNext` + `TakeCurrent`,
// three hundred times. 300 * 1 = 300, 300 mod 251 = 49.
std::string owningEnumeratorAbandoned(const std::string& kw) {
    return
        "module M; "
        "class Item "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Item; "
        "class C "
        "  public static method Gen(): IOwningEnumerator<Item> "
        "    using var buf := new Array<int>(256, 1); "
        "    yield return new Item(buf[0]); "
        "    yield return new Item(buf[1]); "
        "  end Gen; "
        "  public static async method Run(): Task<int> "
        "    " + kw + " en := C.Gen(); "
        "    var n := 0; "
        "    if en.MoveNext() then "
        "      using var it := en.TakeCurrent(); "
        "      n := it.V; "
        "    end; "
        "    return n; "
        "  end Run; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 1..300 do t := t + C.Run().Result; end; "
        "    return t mod 251; "
        "  end Main; "
        "end C;";
}

// An `IOwningAsyncEnumerable<Item>` async iterator, fully consumed through
// `await for`, three hundred times. THIS is the head stdlib §4.13 declares as
// `: IAsyncDisposable` — the one the header's retired justification was wrong
// about — so it is the row that proves the §28.12/§29.7 reasoning, not the
// `IAsyncDisposable` reasoning, is what carries the arm. 300 * 2 = 600,
// 600 mod 251 = 98.
std::string owningAsyncEnumerable(const std::string& kw) {
    return
        "module M; "
        "class Item "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Item; "
        "class C "
        "  public static async method Gen(): IOwningAsyncEnumerable<Item> "
        "    using var buf := new Array<int>(256, 1); "
        "    yield return new Item(buf[0]); "
        "    yield return new Item(buf[1]); "
        "  end Gen; "
        "  public static async method Run(): Task<int> "
        "    " + kw + " src: IOwningAsyncEnumerable<Item> := C.Gen(); "
        "    var n := 0; "
        "    await for var it in src do "
        "      n := n + it.V; "
        "      it.Dispose(); "
        "    end; "
        "    return n; "
        "  end Run; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var i in 1..300 do t := t + C.Run().Result; end; "
        "    return t mod 251; "
        "  end Main; "
        "end C;";
}

// THE RESUMPTION DETECTOR — a VALUE oracle, not a memory one, so it fails on a
// build where ASan reports nothing at all. `Gen` increments a borrowed counter
// cell immediately before each `yield return`; `Run` drives exactly ONE
// `MoveNext` and then lets the binding's scope end inside a `do ... end`
// (§28.10's nested-block form) so the cell can be read afterwards. A correct
// disposal dispatches slot 1 and leaves the cell at 1 → 10 + 1*100 = 110.
// Dispatching slot 2 RESUMES the body, which runs the next `cell[0] := ...`
// statement before yielding → cell 2 → 210. The gap between 110 and 210 is
// one user statement the program never asked to execute.
std::string resumptionDetector(const std::string& kw) {
    return
        "module M; "
        "class C "
        "  public static method Gen(cell: Array<int>): IEnumerator<int> "
        "    cell[0] := cell[0] + 1; "
        "    yield return 10; "
        "    cell[0] := cell[0] + 1; "
        "    yield return 20; "
        "    cell[0] := cell[0] + 1; "
        "    yield return 30; "
        "  end Gen; "
        "  public static async method Run(): Task<int> "
        "    using var cell := new Array<int>(1, 0); "
        "    var v := 0; "
        "    do "
        "      " + kw + " en := C.Gen(cell); "
        "      if en.MoveNext() then v := en.Current; end; "
        "    end; "
        "    return v + cell[0] * 100; "
        "  end Run; "
        "  public static method Main(): int "
        "    return C.Run().Result; "
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

// ── THE FIFTH HEAD: `IOwningEnumerator<T>` (stdlib §4.13's single-use
// owning enumerator, `: IDisposable where T: class`). No wrapper is minted at
// this head and no collection predicate matches it, so pre-fix it fell straight
// to the generic async fallback's word-0 read: 328800 B / 900 allocs over 300
// abandoned executions. // per §28.12, §29.1, §29.7, stdlib §4.13 (issue #3437)
BEZEL_TEST(await_using_iterator_format_disposal_3437,
           await_using_iowningenumerator_head_abandoned_spec_29_1_29_7) {
    BEZEL_EXPECT_EQ(runChecked(owningEnumeratorAbandoned("await using var")),
                    49);
}

BEZEL_TEST(await_using_iterator_format_disposal_3437,
           using_var_iowningenumerator_head_abandoned_control_spec_29_7) {
    BEZEL_EXPECT_EQ(runChecked(owningEnumeratorAbandoned("using var")), 49);
}

// ── THE SIXTH HEAD, and the one that decides the RATIONALE:
// `IOwningAsyncEnumerable<T>` IS declared `: IAsyncDisposable` (stdlib §4.13,
// v0.283, Bezel_Standard_Library.md:2441). It belongs in the arm's set anyway,
// because no `DisposeAsync` is reachable through an iterator-format frame and
// §28.12/§29.7 define the synchronous fallback as the degraded behaviour. This is
// therefore the row that distinguishes the two justifications: it passes under
// the §28.12/§29.7 reading and is a counterexample to the retired
// "none of the six is IAsyncDisposable" one. Pre-fix it leaked 384300 B / 1500
// allocs over 300 executions.
// per §28.12, §29.7, §30.7, stdlib §4.13 (issue #3437)
BEZEL_TEST(await_using_iterator_format_disposal_3437,
           await_using_iowningasyncenumerable_head_spec_28_12_29_7) {
    BEZEL_EXPECT_EQ(runChecked(owningAsyncEnumerable("await using var")), 98);
}

BEZEL_TEST(await_using_iterator_format_disposal_3437,
           using_var_iowningasyncenumerable_head_control_spec_28_10) {
    BEZEL_EXPECT_EQ(runChecked(owningAsyncEnumerable("using var")), 98);
}

// ── THE RESUMPTION DETECTOR. A VALUE oracle: it fails on a build where the
// sanitizers report nothing at all, because the damage it names is not a memory
// fault but the silent execution of a user statement at the disposal point. 210
// pre-fix (the body was resumed past its first `yield return`), 110 with the fix,
// and 110 for the `using var` twin on both compilers.
// per §28.12, §29.3, §29.7, §28.10 (issue #3437)
BEZEL_TEST(await_using_iterator_format_disposal_3437,
           await_using_ienumerator_disposal_does_not_resume_body_spec_29_7) {
    BEZEL_EXPECT_EQ(runChecked(resumptionDetector("await using var")), 110);
}

BEZEL_TEST(await_using_iterator_format_disposal_3437,
           using_var_ienumerator_disposal_does_not_resume_body_control_29_7) {
    BEZEL_EXPECT_EQ(runChecked(resumptionDetector("using var")), 110);
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

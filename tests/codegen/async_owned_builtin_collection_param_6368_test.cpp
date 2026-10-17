// Spec-derived regression tests for issue #6368 — an `owned` PARAMETER of a
// vtable-less BUILT-IN COLLECTION (`owned a: Array<int>`) or of a BUILT-IN
// COLLECTION INTERFACE HEAD (`owned e: IEnumerable<int>`) was left
// UN-RECLAIMED at an ASYNC FRAME's terminal auto-dispose, leaking one backing
// per call.
//
// THE REPROS (the issue's own two arms):
//
//   (2)  async method Take(owned a: Array<int>): Task<int>
//   (5)  async method Take(owned e: IEnumerable<int>): Task<int>
//
// MEASURED fail-without on 85a3cb36 (this tree with the fix reverted,
// `bezelc` rebuilt from the reverted source, the LSan recipe `runAotChecked`
// runs):
//   arm (2) `owned a: Array<int>`  — `SUMMARY: AddressSanitizer: 60000
//     byte(s) leaked in 1000 allocation(s)`, plus the compiler's own
//     `internal: emitOwnedParamCaptureDispose: vtable-less built-in class
//     'Array' has no release arm` warning.
//   arm (5) `owned e: IEnumerable<int>` — the same shape, arm (5)'s
//     `unrecognised owned parameter type` warning, and an equal-per-call leak.
//
// ROOT CAUSE (src/codegen/LlvmBackend_Disposal.cpp,
// `emitOwnedParamCaptureDispose`). This function is the ONE routing decision
// for an owned-parameter capture's terminal dispose, shared by all three frame
// emitters: the plain-async frame (`emitAsyncOwnedParamAutoDispose`,
// LlvmBackend_Async.cpp), the async iterator's Dispose sweep, and the sync
// iterator's owned-param sweep (LlvmBackend_Iterators.cpp). Its arm (2) routes
// every `common::isVtableLessBuiltinClassName(bn)` receiver through
// `emitVtablelessBuiltinRelease`, whose `classifyVtablelessBuiltinRelease`
// name table covers 18 non-collection built-ins (StringBuilder, Random,
// Atomic, Mutex, Channel, ...) and NOT the collection family. A COLLECTION is
// vtable-less by the same predicate and simply had no arm, so the compiler
// said so out loud and left the backing un-reclaimed:
//
//   internal: emitOwnedParamCaptureDispose: vtable-less built-in class
//   'Array' has no release arm; the slot is left un-reclaimed (a bounded
//   leak) — extend emitVtablelessBuiltinRelease to reclaim it; #3157
//
// The COLLECTION's release is not a name-keyed vtable-less release at all: it
// is the runtime free-function cascade (`bz_array_free` / `bz_llist_free` /
// `bz_dict_free`) plus the §28.11 element-slot disposer, which `emitDisposals`
// already emits for an `owned` collection LOCAL and which
// `buildCollectionTempDisposal` already builds for an `owned` collection
// PARAMETER on the callee side. So the fix routes the frame capture through
// the SAME `buildCollectionTempDisposal` shape rather than growing a second,
// divergent collection-release path inside the name table.
//
// WHY THE FRAME AND NOT THE CALLEE: #3033 already taught `registerUnwindOwnedParam`
// to build a `Disposal` for a collection-typed `owned` parameter
// (`buildCollectionTempDisposal(slot, rt)`), so the SYNC spelling of this
// exact defect is already clean — MEASURED on this tree, `static method
// Take(owned a: Array<int>)` called 500× leaks NOTHING (exit code equals the
// spec-computed value). An async method never reaches that callee edge at
// teardown: the value lives in the FRAME, and the frame's terminal auto-dispose
// is the only code that can reclaim it (#6337's own comment records the same
// split for the §31.4 exception population). So the two ends disagreed and
// only the frame end was wrong — this is in-family, and the predicate/machinery
// is the whole defect.
//
// THE INTERFACE HEADS ARE A DIFFERENT QUESTION, and this file treats them as
// one. `owned e: IEnumerable<int>` is NOT the same population as
// `owned a: Array<int>`: §4.12 makes `IEnumerable<T>` a *borrowing
// enumeration interface*, and it is genuinely POLYMORPHIC — the same interface
// head is implemented by `bz_array_t*`-backed `Array<T>`, by
// `bz_dict_t*`/`bz_llist_t*`-backed collections, and by an ordinary vtable-
// bearing USER class (§8.1 makes `class MyE: IEnumerable<int>` legal). The
// callee's static type therefore does not determine the runtime release, and
// the frame cannot soundly pick one.
//
// THE LIST HEADS (`IList<T>` / `IReadOnlyList<T>` / `IOwningList<T>`) ARE
// DECLINED FOR THE SAME REASON, and that is a fix-forward of this very commit.
// An earlier revision of this function DID admit `isBorrowingListInterfaceName`
// / `isOwningListInterfaceName` on the strength of a predicate comment
// claiming "the compiler KNOWS those are array-backed". MEASURED, that claim
// is false: `BuiltinRegistry.cpp`'s `isVtableLessBuiltinClassName` is
// `isReservedBuiltinTypeName(n) && !contains(kBuiltinInterfaceHeads, n)`, and
// `kBuiltinInterfaceHeads` (#2921 / #2923) contains exactly `IList`,
// `IOwningList` and `IReadOnlyList` — as the subset whose runtime value MAY BE
// A USER CLASS, i.e. the exact COMPLEMENT of `isVtableLessBuiltinClassName`.
// §8.1 makes such a user class legal and the compiler accepts it silently.
// Admitting the list heads therefore converted a loud bounded leak into a wild
// free: MEASURED, `async Take(owned x: IList<int>)` receiving `new MyL()` where
// `class MyL: IList<int>` SEGVed in `bz_array_free` under ASan AND under the
// plain oracle (exit 139), where the base tree merely leaked 80 B / 5 allocs
// behind the arm-(5) `internal:` warning. The list heads are removed here, and
// rows (5a)/(5b)/(5c)/(5d) below pin the removal.
//
// The removal also makes the frame EXACTLY as NARROW as the callee end it
// mirrors: `registerUnwindOwnedParam`'s collection branch admits only
// `isLinkedListTypeName(rt) || isDictTypeName(rt) || isCollectionTypeName(rt)`
// — never the list interfaces — so the frame previously WIDENED past the callee
// in precisely the unsound direction. Excluding the list heads restores exact
// frame/callee parity. The price is that the array-backed list-head spelling
// now DECLINES loudly too (row 5d): a leak the compiler announces beats a
// SEGV it does not. A complete fix needs a runtime kind tag; tracked on the
// issue that also covers the `owned` FIELD site.
//
// Rows (5a)-(5d) below PIN that polymorphism rather than assert a fix for it:
// they show the user-class members (which must go through the vtable) and the
// array members (which would need `bz_array_free`) are BOTH admitted at this
// same site, so a fix that picks one statically would silently corrupt the
// other. See the issue and `handback` for the `spec-question` this raises.
//
// Spec: §28.3 *Unbound owned temporaries* and the must-use auto-drop, §28.8
// (only an owned reference carries the disposal obligation), §28.9
// (runtime-managed types; the compiler frees an undischarged value at its
// owner's release point), §28.11 (collection element ownership — the
// element-slot policy table and the borrowing/owning split), §30.1 (the frame
// disposes an `owned` parameter that never moved out), §29.8 (an iterator's
// state machine disposes its owned parameters at iterator-Dispose), §8.1 (a
// user class may implement a built-in interface head), §4.12/§4.13 via stdlib
// §4.1–§4.3 (the list interfaces are array-backed; the enumeration interfaces
// are not). Revision v0.338.

#include <string>

#include "support/aot_support.hpp"   // runAot — AOT oracle
                                  // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A leak / double-free / use-after-free flips `ran` to false -> -999, distinct
// from any legal exit value. `expectRanWith` is the ONLY `runAotChecked` call
// site in this file, and it does NOT absorb a toolchain skip: a skip makes the
// harness return `ran = false` (aot_detail::markInfra), which the
// `BEZEL_EXPECT_TRUE(r.ran)` below turns into a row FAILURE rather than a
// silent pass. There is no `wasSkipped(r)` / `if (!r.ran) return;` here.
//
// The rows assert BOTH halves: `ran` says the program ran to normal
// termination under ASan/LSan (no leak, no memory error), and the exit code
// says it computed the spec-derived value. Either half alone is satisfiable
// vacuously.
//
// 500 iterations is far above the 50-500 one-shot band: the leak is per-call,
// and a single-shot repro measures exactly one call. Every row accumulates a
// real value from 500 distinct heap objects, so a partial discharge shows up as
// a wrong exit code as well as an LSan report.
void expectRanWith(const std::string& src, int expected) {
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, expected);
}


// The `IEnumerable<T>` polymorphism rows below need a DIFFERENT oracle from
// `expectRanWith`, and the difference is the point. The
// `IEnumerable<T>` / `IReadOnlyCollection<T>` / `ICollection<T>` /
// `IOwningCollection<T>` heads are deliberately NOT admitted by
// `emitOwnedCaptureCollectionRelease` — no runtime kind tag distinguishes
// their two possible runtime kinds at an `owned` capture, so no static
// release is sound for them and arm (5) keeps declining them loudly. That
// leaves a leak, and a leak is exactly what a `runAotChecked` row would fail
// on. Asserting the leak is PRESENT would invert the row the moment the
// population is fixed.
//
// So these rows use the PLAIN oracle and assert the property that is true on
// every side of that fix: normal termination with the spec-derived value. Each
// of these rows is red in the direction its own shape mis-releases — the
// USER-CLASS rows die in `bz_array_free` reading a vtable pointer as a length
// (`ran` flips to false, exit 139 under the plain oracle); the ARRAY-BACKED
// rows die because the frame's free collides with the caller's still-live
// `owned` temporary from `Make()`. Still no skip-absorbing guard: a
// skip/toolchain failure also yields `ran == false`, which fails the row here
// too.
void expectRanNormally(const std::string& src, int expected) {
    auto r = bezel::test::runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, expected);
}

}  // namespace

// §28.3 / §28.11 / §30.1 (#6368) — THE HEADLINE SHAPE, arm (2). `Take`'s
// `owned a: Array<int>` is never moved / returned / disposed, so §28.3
// auto-drops it at the end of the method and the async frame's terminal
// auto-dispose is the only code that can reclaim the `bz_array_t` backing.
//
// The element type `int` is a §28.11 VALUE slot in a BORROWING collection, so
// the correct release is `bz_array_free(arr, /*elemDisposer=*/null)` — the
// collection does NOT own its elements, and the backing storage is "part of the
// instance's allocation, released with it". That is the row that pins the
// element-disposer half of the answer (a row with a CLASS element follows).
//
// 500 x 3 = 1500; 1500 mod 251 = 245.
// Pre-fix: `SUMMARY: AddressSanitizer: 60000 byte(s) leaked in 1000
//           allocation(s)`; exit 99 -> -999.
// Post-fix: LSan-clean, 245.
BEZEL_TEST(codegen_async_owned_builtin_collection_param_6368,
           async_owned_Array_int_param_backing_freed_spec_28_11) {
    std::string src =
        "module M; "
        "class Q "
        "  public static async method Take(owned a: Array<int>): Task<int> "
        "    await Task.Delay(1); "
        "    return a.Count; "
        "  end Take; "
        "end Q; "
        "class P public static async method Main(): Task<int> "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    var v := await Q.Take([1, 2, 3]); "
        "    t := t + v; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanWith(src, 245);
}

// §28.11 (#6368) — THE ELEMENT-OWNERSHIP HALF. `Array<Cell>` where `Cell` is a
// USER CLASS: in a BORROWING collection the element slot is a §28.11 BORROW
// slot ("element disposal is the arrangement of whoever owns the elements and
// never happens through the borrowing collection"), so the release is STILL
// `bz_array_free(arr, null)` — the array must NOT try to dispose elements it
// merely borrows. Passing the owning-family disposer here would dispose every
// borrowed `Cell`: an ASan double free / use-after-free, which is strictly
// worse than the leak this issue is about.
//
// The three `Cell`s are minted inside `Make` and each dies at its own K8
// full-expression boundary (§28.3 — that is precisely why the compiler's
// fresh-store lint fires on this shape), so the ARRAY is the sole surviving
// holder of the backing; if the release wrongly cascaded into the elements it
// would free them a second time.
//
// This is the row that ANSWERS the design question "is `bz_array_free` the
// correct release for `Array<T>`?": yes, but with the §28.11 element-slot
// policy's disposer for the element's CATEGORY — `null` for a borrow slot —
// and it is NOT the same wiring as the `StringBuilder` arm (which owns a +1
// per string part and passes the string-release thunk). The classification is
// `buildCollectionTempDisposal`'s, shared with the `using var` and callee
// spellings; this row is what pins that it picks the BORROWING policy here.
//
// 500 x 3 = 1500; 1500 mod 251 = 245.
// Post-fix: LSan-clean, 245.
BEZEL_TEST(codegen_async_owned_builtin_collection_param_6368,
           async_owned_Array_class_elem_is_borrowed_not_disposed_spec_28_11) {
    std::string src =
        "module M; "
        "class Cell "
        "  private _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public method N(): int return _n; end N; "
        "end Cell; "
        "class Q "
        "  public static method Make(): owned Array<Cell> "
        "    var a: Array<Cell> := new Array<Cell>(); "
        "    a.Add(new Cell(9)); "
        "    a.Add(new Cell(8)); "
        "    a.Add(new Cell(7)); "
        "    return a; "
        "  end Make; "
        "  public static async method Take(owned a: Array<Cell>): Task<int> "
        "    await Task.Delay(1); "
        "    return a.Count; "
        "  end Take; "
        "end Q; "
        "class P public static async method Main(): Task<int> "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    var v := await Q.Take(Q.Make()); "
        "    t := t + v; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanWith(src, 245);
}

// §28.11 / §4.8 (#6368) — the DICT-backed sibling, so the fix is not an
// `Array`-only special case. A `Dictionary<int, string>` is `bz_dict_t*`, not
// `bz_array_t*`: the backing and the entries are freed by `bz_dict_free`, and
// the §28.11 string element carries a co-owned +1 the dictionary releases. A
// fix that routed every vtable-less built-in collection through
// `bz_array_free` would corrupt this row (a `free()` on a `bz_dict_t`).
//
// The `Make` factory returns a fresh `owned Dictionary<int, string>` so the
// callee genuinely owns it (§28.8), and `d.Count` reads it so the callee
// genuinely holds it.
//
// 500 x 1 = 500; 500 mod 251 = 249.
// Post-fix: LSan-clean, 249.
BEZEL_TEST(codegen_async_owned_builtin_collection_param_6368,
           async_owned_Dictionary_param_backing_freed_spec_28_11) {
    std::string src =
        "module M; "
        "class Q "
        "  public static method Make(): owned Dictionary<int, string> "
        "    var d := new Dictionary<int, string>(); "
        "    d.Add(1, \"a\"); "
        "    return d; "
        "  end Make; "
        "  public static async method Take(owned d: Dictionary<int, string>): Task<int> "
        "    await Task.Delay(1); "
        "    return d.Count; "
        "  end Take; "
        "end Q; "
        "class P public static async method Main(): Task<int> "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    var v := await Q.Take(Q.Make()); "
        "    t := t + v; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanWith(src, 249);
}

// §28.3 / §28.9 (#6368) — OVER-RELEASE GUARD, the DISCHARGE half. The callee
// disposes the collection EXPLICITLY (§28.2: a written `a.Dispose()` is a
// release EARLY) and then returns normally. The frame's terminal auto-dispose
// must then find a NULLED slot; freeing again is an ASan double free. This is
// the row that would catch the opposite defect introduced by widening arm (2)
// — one that frees a value whose owner has already released it. Note this row
// is vacuous on the pre-fix tree for the WRONG reason (arm (2) declines to
// free at all, so there is nothing to double-free); it earns its keep by
// failing if the fix over-reaches.
//
// 500 x 3 = 1500; 1500 mod 251 = 245.
// Post-fix: LSan-clean, 245.
BEZEL_TEST(codegen_async_owned_builtin_collection_param_6368,
           async_owned_Array_param_explicit_dispose_then_return_no_double_free) {
    std::string src =
        "module M; "
        "class Q "
        "  public static async method Take(owned a: Array<int>): Task<int> "
        "    var n := a.Count; "
        "    a.Dispose(); "
        "    await Task.Delay(1); "
        "    return n; "
        "  end Take; "
        "end Q; "
        "class P public static async method Main(): Task<int> "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    var v := await Q.Take([1, 2, 3]); "
        "    t := t + v; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanWith(src, 245);
}

// §28.8 (#6368) — OVER-RELEASE GUARD, the OWNERSHIP half. A plain (borrow)
// `a: Array<int>` parameter has the SAME vtable-less built-in collection type
// as the owned row above, so if the widened arm were driven by the TYPE alone
// rather than by type AND the `owned` modifier, this row would free a value
// the callee does NOT own: the caller's `using var` is still the holder
// (§28.8) and would free the same backing a second time — an ASan double
// free. The `owned` precondition is asserted at BOTH ends of the chain.
//
// 500 x 3 = 1500; 1500 mod 251 = 245.
// Post-fix: LSan-clean, 245.
BEZEL_TEST(codegen_async_owned_builtin_collection_param_6368,
           async_borrow_param_Array_is_never_freed_spec_28_8) {
    std::string src =
        "module M; "
        "class Q "
        "  public static async method Borrow(a: Array<int>): Task<int> "
        "    await Task.Delay(1); "
        "    return a.Count; "
        "  end Borrow; "
        "end Q; "
        "class P public static async method Main(): Task<int> "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    using var a := new Array<int>(); "
        "    a.Add(1); a.Add(2); a.Add(3); "
        "    var v := await Q.Borrow(a); "
        "    t := t + v; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanWith(src, 245);
}

// §29.8 / §28.3 (#6368) — the ASYNC ITERATOR's Dispose sweep, the SECOND
// emitter routed by the same function (LlvmBackend_Async.cpp:4566). Same
// declaration, same routing decision, second caller — so "the fix closes the
// populations at ONE site" is an ASSERTED claim, not an assumption. The
// consumer runs the iterator to exhaustion, so the disposal lands on the
// exhaustion path of Dispose.
//
// 500 x 3 = 1500; 1500 mod 251 = 245.
// Post-fix: LSan-clean, 245.
BEZEL_TEST(codegen_async_owned_builtin_collection_param_6368,
           async_iterator_owned_Array_param_backing_freed_spec_29_8) {
    std::string src =
        "module M; "
        "class Q "
        "  public static async method Take(owned a: Array<int>): IAsyncEnumerable<int> "
        "    yield return a.Count; "
        "  end Take; "
        "end Q; "
        "class P public static async method Main(): Task<int> "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    await for var x in Q.Take([1, 2, 3]) do "
        "      t := t + x; "
        "    end; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanWith(src, 245);
}

// §29.8 / §28.3 (#6368) — the SYNC iterator's owned-param sweep, the THIRD
// emitter routed by the same function (LlvmBackend_Iterators.cpp). Same
// declaration, third caller — so all three loops are pinned by this file.
//
// 500 x 3 = 1500; 1500 mod 251 = 245.
// Post-fix: LSan-clean, 245.
BEZEL_TEST(codegen_async_owned_builtin_collection_param_6368,
           sync_iterator_owned_Array_param_backing_freed_spec_29_8) {
    std::string src =
        "module M; "
        "class Q "
        "  public static method Take(owned a: Array<int>): IEnumerable<int> "
        "    yield return a.Count; "
        "  end Take; "
        "end Q; "
        "class P public static method Main(): int "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    for var x in Q.Take([1, 2, 3]) do "
        "      t := t + x; "
        "    end for; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanWith(src, 245);
}

// §28.3 / §28.11 (#6368) — the CALLEE-SIDE control for the sync spelling.
// #3033 already taught `registerUnwindOwnedParam` to build a collection
// `Disposal` (`buildCollectionTempDisposal(slot, rt)`), so the NON-async
// spelling of the headline shape is ALREADY clean. This row is green on the
// pre-fix tree and MUST STAY green: it is the row that proves the fix is
// scoped to the frame path and did not disturb the callee path it mirrors.
// MEASURED on 85a3cb36: this program leaks nothing.
//
// 500 x 3 = 1500; 1500 mod 251 = 245.
// Pre-fix AND post-fix: LSan-clean, 245.
BEZEL_TEST(codegen_async_owned_builtin_collection_param_6368,
           sync_owned_Array_param_callee_side_stays_clean_spec_28_3) {
    std::string src =
        "module M; "
        "class Q "
        "  public static method Take(owned a: Array<int>): int "
        "    return a.Count; "
        "  end Take; "
        "end Q; "
        "class P public static method Main(): int "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    t := t + Q.Take([1, 2, 3]); "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanWith(src, 245);
}

// §8.1 / stdlib §4.12 (#6368) — THE POLYMORPHISM ROW for arm (5), the
// VTABLE-BEARING member. `MyE` is an ordinary USER class implementing
// `IEnumerable<int>` (§8.1 makes this legal; the repo's own
// `builtin_interpolation_iface_head_2969_test.cpp` already relies on it). Its
// correct release at an `owned e: IEnumerable<int>` capture is vtable slot-2
// dispatch — NOT `bz_array_free`, which would read the vtable pointer as a
// length.
//
// This row therefore uses the PLAIN oracle (see `expectRanNormally`) rather
// than `expectRanWith`: the non-list collection interface heads are
// deliberately not admitted by `emitOwnedCaptureCollectionRelease`, so this
// population's leak is still open and a checked row would be asserting it.
//
// It does NOT iterate the parameter, so the row measures the TEARDOWN and
// nothing else. 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_async_owned_builtin_collection_param_6368,
           async_owned_IEnumerable_user_class_capture_runs_normally_8_1) {
    std::string src =
        "module M; "
        "class MyE: IEnumerable<int> "
        "  private owned _a: Array<int>; "
        "  public constructor() _a := new Array<int>(1); end constructor; "
        "  public method GetEnumerator(): IEnumerator<int> "
        "    return _a.GetEnumerator(); "
        "  end GetEnumerator; "
        "end MyE; "
        "class Q "
        "  public static async method Take(owned e: IEnumerable<int>): Task<int> "
        "    await Task.Delay(1); "
        "    return 1; "
        "  end Take; "
        "end Q; "
        "class P public static async method Main(): Task<int> "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    var v := await Q.Take(new MyE()); "
        "    t := t + v; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanNormally(src, 249);
}

// stdlib §4.12 / §15.4 (#6368) — the ARRAY-BACKED member of the SAME
// interface head, and the companion half of the pair that makes arm (5)'s
// polymorphism a MEASURED property of this site rather than a claim in a
// comment: `Make` returns an `Array<int>` widened to `IEnumerable<int>`, so
// the runtime value at the capture is a `bz_array_t*`, not a vtable, and its
// correct release is the OPPOSITE of the row above's. The declared type
// distinguishes neither — which is why no static choice is sound here and
// arm (5) still declines.
//
// Between them the two rows establish that both runtime kinds traverse the
// SAME teardown site, which is what makes "no single static release is sound
// at this site" a demonstrated property of the code and not an assumption.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_async_owned_builtin_collection_param_6368,
           async_owned_IEnumerable_array_backed_capture_runs_normally_4_12) {
    std::string src =
        "module M; "
        "class Q "
        "  public static method Make(): owned IEnumerable<int> "
        "    var a: Array<int> := [1, 2, 3]; "
        "    return a; "
        "  end Make; "
        "  public static async method Take(owned e: IEnumerable<int>): Task<int> "
        "    await Task.Delay(1); "
        "    return 1; "
        "  end Take; "
        "end Q; "
        "class P public static async method Main(): Task<int> "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    var v := await Q.Take(Q.Make()); "
        "    t := t + v; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanNormally(src, 249);
}

// §8.1 (#6368) — FIX-FORWARD: the LIST heads are declined too, and this is the
// row that pins it. `class MyL: IList<int>` is an ordinary USER class
// implementing a built-in interface head — §8.1 makes it legal and the
// compiler accepts it with NO diagnostic. Because `kBuiltinInterfaceHeads`
// (#2921 / #2923) contains `IList`, `isVtableLessBuiltinClassName` is FALSE
// for it, so this capture can only be released by vtable dispatch; routing it
// to `bz_array_free` would read the vtable pointer as a length.
//
// MEASURED fail-without for the list-head admission: with
// `isBorrowingListInterfaceName(pt) || isOwningListInterfaceName(pt)` back in
// the guard, this program is
//   `==ERROR: AddressSanitizer: SEGV on unknown address` in
//   `__asan::Allocator::Deallocate` <- `free` <- `bz_array_free`
//   (bezel_runtime_collections.cpp:351),
// and under the PLAIN oracle `Segmentation fault (core dumped)` / exit 139.
// With the list heads removed it terminates normally with the spec value. The
// `expectRanWith` oracle would be wrong here — the fix deliberately restores
// the loud arm-(5) decline, so the leak is open on this spelling and a checked
// row would be asserting a leak that a future runtime kind tag will remove.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_async_owned_builtin_collection_param_6368,
           async_owned_IList_user_class_capture_is_declined_not_freed_8_1) {
    std::string src =
        "module M; "
        "class MyL: IList<int> "
        "  private owned _a: Array<int>; "
        "  public constructor() _a := new Array<int>(1); end constructor; "
        "  public method GetEnumerator(): IEnumerator<int> "
        "    return _a.GetEnumerator(); "
        "  end GetEnumerator; "
        "end MyL; "
        "class Q "
        "  public static async method Take(owned x: IList<int>): Task<int> "
        "    await Task.Delay(1); "
        "    return 1; "
        "  end Take; "
        "end Q; "
        "class P public static async method Main(): Task<int> "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    var v := await Q.Take(new MyL()); "
        "    t := t + v; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanNormally(src, 249);
}

// §8.1 (#6368) — FIX-FORWARD: the same pin for the OWNING list head.
// `IOwningList` is in `kBuiltinInterfaceHeads` (#2921 / #2923) for exactly the
// same reason as `IList`, so the same vtable-bearing member arrives here and
// the same `bz_array_free` mis-release kills the process. MEASURED
// fail-without for the list-head admission: `SEGV in bz_array_free` under ASan,
// exit 139 under the plain oracle.
//
// The element type is `Cell` rather than `int` so this row also exercises the
// OWNING element family: `buildCollectionTempDisposal` would pass the §28.11
// owning cascade for a `Cell` element, and a mis-admitted list head therefore
// has a SECOND way to be wrong on the same program — it would dispose `Cell`s
// the owning-list borrowing contract says it does not own.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_async_owned_builtin_collection_param_6368,
           async_owned_IOwningList_user_class_capture_is_declined_not_freed_8_1) {
    std::string src =
        "module M; "
        "class Cell "
        "  private _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public method N(): int return _n; end N; "
        "end Cell; "
        "class MyL: IOwningList<Cell> "
        "  private owned _a: Array<Cell>; "
        "  public constructor() _a := new Array<Cell>(); end constructor; "
        "  public method GetEnumerator(): IEnumerator<Cell> "
        "    return _a.GetEnumerator(); "
        "  end GetEnumerator; "
        "end MyL; "
        "class Q "
        "  public static async method Take(owned x: IOwningList<Cell>): Task<int> "
        "    await Task.Delay(1); "
        "    return 1; "
        "  end Take; "
        "end Q; "
        "class P public static async method Main(): Task<int> "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    var v := await Q.Take(new MyL()); "
        "    t := t + v; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanNormally(src, 249);
}

// stdlib §4.12 / §15.4 (#6368) — FIX-FORWARD, the array-backed half of the
// LIST heads. This is the spelling the removed admission was written FOR: an
// `Array<int>` widened to `IList<int>`, so the runtime value really is a
// `bz_array_t*` and `bz_array_free` really would be the right free. It is
// DECLINED anyway, and deliberately so: the declared type does not distinguish
// it from the row above's vtable-bearing `MyL`, and there is no runtime kind
// tag at this site to tell them apart. Declining keeps both spellings on the
// loud arm-(5) path; admitting either would be a wild free for the other.
//
// MEASURED: this program leaks 60,000 B / 1,000 allocs at 500 iterations behind
// the arm-(5) `internal:` warning — on the base tree AND on this tree. The
// plain oracle is the right one for exactly that reason; a `runAotChecked` row
// would be asserting the leak is present, which inverts the moment a runtime
// kind tag lands.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_async_owned_builtin_collection_param_6368,
           async_owned_IList_array_backed_capture_is_declined_not_freed_4_12) {
    std::string src =
        "module M; "
        "class Q "
        "  public static method Make(): owned IList<int> "
        "    var a: Array<int> := [1, 2, 3]; "
        "    return a; "
        "  end Make; "
        "  public static async method Take(owned x: IList<int>): Task<int> "
        "    await Task.Delay(1); "
        "    return 1; "
        "  end Take; "
        "end Q; "
        "class P public static async method Main(): Task<int> "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    var v := await Q.Take(Q.Make()); "
        "    t := t + v; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanNormally(src, 249);
}

// §28.11 / §30.1 (#6368) — the CROSS-COMMIT shape: a GENERIC async method whose
// type parameter is bound to a built-in COLLECTION, so #6367's
// `pt = resolveTypeParamRef(pt)` and #6368's collection arm both fire at the
// SAME routing decision. `Q.Take<Array<int>>([1,2,3])` binds `T = Array<int>`;
// the frame's `pt` is a bare type parameter until the substitution resolves it,
// and only then does the collection arm see `Array<int>`.
//
// This row is LSan-CLEAN on this tree and so uses the checked oracle — it is
// the assertion that the two fixes compose without double-freeing the same
// backing (the substitution resolves once, and the collection arm consumes it
// once). CHARACTERISED, not fixed: the arm-(5) `internal:` warning is still
// emitted, but from the UNINSTANTIATED TEMPLATE copy of the generic method
// where `genericSubstSemaTy_` is empty; the per-instantiation copy resolves and
// frees, which is why there is no leak. That warning is recorded on the issue
// rather than coded around here.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_async_owned_builtin_collection_param_6368,
           async_owned_generic_Array_param_backing_freed_spec_28_11) {
    std::string src =
        "module M; "
        "class Q "
        "  public static async method Take<T>(owned a: T): Task<int> "
        "    await Task.Delay(1); "
        "    return 1; "
        "  end Take; "
        "end Q; "
        "class P public static async method Main(): Task<int> "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    var v := await Q.Take<Array<int>>([1, 2, 3]); "
        "    t := t + v; "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
    expectRanWith(src, 249);
}

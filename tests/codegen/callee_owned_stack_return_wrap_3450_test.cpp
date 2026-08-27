// Codegen / use-after-free — fix #3450 (§28.2 / §28.9 / §28.11; stdlib §4.11,
// v0.283): the callee-owned `Stack`/`OwningStack` RETURN coercion built a
// BORROWING enumerator over the source's backing array and then freed that
// array immediately, handing the caller an enumerator over memory the callee
// had just reclaimed.
//
// THE DEFECT. `wrapAsIEnumerableForTarget`'s `srcOwnedByCallee` arm read:
//
//     if (an == "Stack" || an == "OwningStack") {
//         llvm::Value* wrap = wrapStackAsIEnumerator(v, targetTy);
//         freeCollectionSourceNow(v, actualTy);
//         return wrap;
//     }
//
// justified by a comment that had stopped being true: "A Stack/OwningStack
// source has no OWNING-wrap variant in this fix". #2987 built
// `wrapOwnedStackAsIEnumerator` for exactly this shape at v0.240, and the
// local-decl site in `LlvmBackend_Setup.cpp` has used it ever since. This is
// byte for byte the shape #3354 identified and fixed for the `LinkedList` twin
// two arms further down ("The previous shape (plain borrow wrap +
// `freeLinkedListSourceNow` immediately after) freed the backing right here, so
// the caller's `for x in e do ... end` walked ... already-freed memory"); the
// stack twin was not updated. That is #3419's pattern — two sites spelling the
// same decision, one of them left behind.
//
// ⚠ A #2633 ORACLE SPLIT, IN ONE PROGRAM. Measured on 0c5ecd1, `Make():
// IEnumerable<int>` returning a callee-owned `Stack<int>` of 1, 2, 3:
//
//   plain build      NONDETERMINISTIC — `order=0` (a SILENT zero-iteration
//                    loop, exit 0) or SIGSEGV, run to run, out of ONE binary.
//                    Re-measured 5x on the same `origin/main` executable while
//                    addressing the review: 3 crashes, 2 silent `order=0`. An
//                    earlier revision of this note said "`order=0`" flatly;
//                    that is one of the two outcomes, not the outcome.
//   ASan build       SEGV on unknown address 0x11, READ, in
//                    `bz$stackenum$i32$Current` <- `bz$M.Main$Run`
//
// Neither oracle alone tells the whole story and the plain one does not even
// fail, so rows (A) and (B) run the SAME program on both — a plain-build
// value assertion and a sanitized no-fault assertion.
//
// THE FIX. Mirror #3354 exactly: use the existing owning wrap and drop the
// immediate free. `wrapOwnedStackAsIEnumerator`'s slot-1 Dispose frees the
// backing AND the wrapper frame at the CALLER's scope end — after iteration,
// not before it — and its slot 4 stamps fresh NON-owning clones, so a per-loop
// enumerator frees only its own struct (#2987's protocol). The source local's
// own reclaim is suppressed at the return edge, so the array is freed exactly
// once; rows (A)/(B)/(G) under ASan+LSan are the no-double-free and no-leak
// halves of that claim.
//
// THE SECOND HALF THE ISSUE FLAGGED. #2987's Dispose passed a hardcoded NULL
// element disposer, argued from "the source is an unbound `new` by
// construction, so it is EMPTY". That argument holds at the local-decl site it
// was written for and is FALSE at this face — the repro pushes three elements
// before the return. The array twin `wrapOwnedArrayAsIEnumerable` has DERIVED
// its disposer from the enumerator's element type since #2710; the two wraps
// answering the same question differently is the same duplicated-predicate
// pattern, and here the disagreement is a leak. Both now ask one helper,
// `ownedWrapElementDisposer`. Row (G) is the measurement: with the disposer
// forced back to null on a scratch compiler, that row leaks 15200 B in 800
// allocations at 200 iterations; with the fix it is LSan-clean.
//
// ★ THE CACHE KEY, AND THE WILD FREE ALREADY ON MAIN. A disposer that varies
// with the SEMA element type, installed in a function whose cache key varies
// only with the LOWERED one, hands the second element type the first one's
// Dispose — `string`, any class, `IGrouping` and every function type all lower
// to `ptr`. The disposer's identity is therefore part of the key, in both
// wraps. That is NOT merely defensive hardening for the arm this fix touches:
// `wrapOwnedArrayAsIEnumerable` has derived its disposer since #2710 **with no
// key segment at all**, so the collision is LIVE ON `origin/main` for the array
// family, and this commit closes it.
//
// The element type that exposes it is a FUNCTION type. A class-typed partner
// only degrades to a silent field decrement (`bz_string_release` touches a
// refcount at an offset and the runtime is not ASan-instrumented); a
// `(int) -> int` element points that same thunk at `.text`, and the release
// becomes `free()` on a function pointer. It also carries a null disposer and
// no disposal obligation of its own, which is what makes it a usable partner at
// all — there is no confounding element leak to separate the collision from.
// Measured on `origin/main`, row (H)'s program under ASan:
//
//     SEGV ... WRITE
//       #1 free
//       #2 bz_string_release  src/runtime/bezel_runtime_refcounting.cpp:137:5
//       #3 bz$strElemRelease
//       #4 bz_array_free      src/runtime/bezel_runtime_collections.cpp:295:27
//       #5 bz$arrenumown$ptr$Dispose
//       #6 bz$M.Main$Run
//
// The method names `AaaStr` / `ZzzFn` are load-bearing, not decoration: they
// order the two mints so the STRING thunk wins the shared name and the function
// elements inherit it. Reverse the order and the collision degrades to a leak.
//
// An earlier revision of this file declined to ship a row here, reasoning that
// no oracle separated the collision from an unrelated pre-existing leak. That
// was true of the LSan oracle ONLY, and this file had already reached for the
// same escape twice (row (C) here, row (F) in the #3461 file): drop to `runAot`
// and assert the PLAIN-build exit status. Rows (H)/(I) do exactly that.
//
// COVERAGE.
//   (A) The #3450 pin, plain build — the value the silent failure produced.
//   (B) The same program sanitized — the fault the plain build did not show.
//   (C) `OwningStack<Item>` at the same face. ⚠ `runAot`, not `runAotChecked`:
//       an `OwningX<T>` callee-owned source coerced to a BORROWING
//       `IEnumerable<T>` head leaks its elements (the wrap's disposer is the
//       ClassBorrow row's null, because the head is borrowing). Measured
//       IDENTICAL for the `OwningArray` twin on this build — a pre-existing,
//       family-wide gap in the safe (under-free) direction, filed separately.
//   (D) The `IOwningEnumerable<Item>` head at the SAME arm — a further cell of
//       the grid, and a live SEGV on 0c5ecd1 (the borrowing wrap's drain divert
//       fired, then the immediate free pulled the array out from under it).
//   (E)/(F) Verified negatives at the same arm: `Queue<int>` and `Array<int>`
//       callee-owned returns keep front-to-back order and stay clean. The arm
//       edited is shared; these are the rows that say the edit was confined to
//       the stack key.
//   (G) The element-disposer row — a `Stack<string>` callee-owned return,
//       loop-amplified 200x under LSan. A one-shot row here is worthless: the
//       leak is 76 B per iteration and a single allocation can still be
//       reachable at exit.
//   (H) The CACHE-KEY row, ARRAY family — a true regression row. It FAILS on
//       `origin/main` (`total=22`, then rc 139, deterministically 5/5) and
//       passes here. This is the pre-existing #2710 wild free above.
//   (I) The same shape at the STACK family — a FORWARD RATCHET, labelled as
//       such. It also fails on `origin/main`, but for #3450's own reason (that
//       arm never reaches the disposer at all: 3/5 SIGSEGV, 2/5 silent
//       `total=0`), so main does not isolate the collision for this family.
//       What isolates it is a key-removed build of THIS tree, on which (H) and
//       (I) both SEGV 5/5 while every other row in this file stays green.
//
// ⚠ Rows (H)/(I) are `runAot`, and must stay so. Under `runAotChecked` both
// leak 80 B in 2 allocations WITH the fix — the function-typed elements are
// never released, because the wrap's slot-1 disposer is derived from the
// BORROWING head. Same root cause as row (C)'s note, a different element
// category; recorded on the issue filed for that gap. The observable these rows
// assert is the plain-build exit status, which is where the wild free lands.
//
// ORACLE. `runAotChecked` compiles with AddressSanitizer + LeakSanitizer, so
// `r.ran` is simultaneously a no-fault and a no-leak assertion.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is
// absent: runAotChecked returns ran=false with a "SKIP" diagnostic.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// The issue's own repro program, verbatim in shape: a method whose local is an
// unbound `new Stack<int>()` (which makes `returnSourceIsCalleeOwnedCollection`
// true) returned through an `IEnumerable<int>` head, walked by the caller.
// (Never put a C++ `//` comment inside these literals: it is lexed as Bezel
// source and comments out the rest of the program. Bezel's own comment forms
// are `//`, `/* ... */` and `///` -- spec §1.5, which is exhaustive.)
const char* const kIntStackRepro =
    "module M; "
    "class Main "
    "  public static method Make(): IEnumerable<int> "
    "    var s := new Stack<int>(); "
    "    s.Push(1); "
    "    s.Push(2); "
    "    s.Push(3); "
    "    return s; "
    "  end Make; "
    "  public static method Run(): int "
    "    using var e := Make(); "
    "    var order := 0; "
    "    for var x in e do "
    "      order := order * 10 + x; "
    "    end for; "
    "    Console.WriteLine($\"order={order}\"); "
    "    return 0; "
    "  end Run; "
    "  public static method Main(): int "
    "    return Run(); "
    "  end Main; "
    "end Main;";

}  // namespace

// ---------------------------------------------------------------------------
// (A) The pin, PLAIN build — the silent wrong answer.
//   per stdlib §4.11 "Iteration order" (v0.283): "All four types iterate (via
//   `IReadOnlyCollection<T>`) in take order — a stack top-to-bottom (pop
//   order), a queue front-to-back (dequeue order); the owning forms iterate
//   identically, yielding borrows."  §28.9 (the RETURN coercion), §28.2
// ---------------------------------------------------------------------------
BEZEL_TEST(callee_owned_stack_return_wrap_3450,
           callee_owned_stack_return_iterates_in_pop_order_4_11) {
    JitResult r = runAot(kIntStackRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    // Pre-fix on 0c5ecd1: "order=0" — the loop ran ZERO iterations over the
    // freed backing, and exited 0. 3, 2, 1 is stdlib §4.11's pop order.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "order=321"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "order=0"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (B) The SAME program, SANITIZED — the fault the plain build hides.
//   per §28.2 (dispose exactly once), §28.9; #2633 (the oracle split)
// ---------------------------------------------------------------------------
BEZEL_TEST(callee_owned_stack_return_wrap_3450,
           callee_owned_stack_return_does_not_free_the_backing_early_28_9) {
    JitResult r = runAotChecked(kIntStackRepro);
    if (wasSkipped(r)) return;
    // Pre-fix on 0c5ecd1: AddressSanitizer SEGV on unknown address 0x11, READ,
    // in bz$stackenum$i32$Current. `r.ran` is also the no-leak and
    // no-double-free assertion: the backing is now freed exactly once, by the
    // wrap's slot-1 Dispose at the caller's scope end.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "order=321"));
}

// ---------------------------------------------------------------------------
// (C) `OwningStack<Item>` at the same face — the class-element twin.
//   per stdlib §4.11, §28.9
//
// ⚠ `runAot`, deliberately. See the header note: an `OwningX<T>` source at a
// BORROWING `IEnumerable<T>` head leaks its elements on this build, identically
// for the `OwningArray` twin. That is a separate, pre-existing gap in the
// under-free direction; the observable asserted here is the one this fix moves.
// ---------------------------------------------------------------------------
BEZEL_TEST(callee_owned_stack_return_wrap_3450,
           callee_owned_owning_stack_return_iterates_in_pop_order_4_11) {
    const std::string src =
        "module M; "
        "class Item "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Item; "
        "class Main "
        "  public static method Make(): IEnumerable<Item> "
        "    var s := new OwningStack<Item>(); "
        "    s.Push(new Item(1)); "
        "    s.Push(new Item(2)); "
        "    return s; "
        "  end Make; "
        "  public static method Run(): int "
        "    using var e := Make(); "
        "    var order := 0; "
        "    for var x in e do "
        "      order := order * 10 + x.V; "
        "    end for; "
        "    Console.WriteLine($\"order={order}\"); "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    // Pre-fix on 0c5ecd1: "order=0", the same silent zero-iteration walk.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "order=21"));
    BEZEL_EXPECT_FALSE(has(r.stdoutText, "order=0"));
}

// ---------------------------------------------------------------------------
// (D) The `IOwningEnumerable<Item>` head at the SAME arm — the further cell.
//   per §29.2 (owning desugaring), stdlib §4.11 / §4.13, §28.9
// ---------------------------------------------------------------------------
BEZEL_TEST(callee_owned_stack_return_wrap_3450,
           callee_owned_stack_at_the_owning_head_does_not_fault_29_2) {
    const std::string src =
        "module M; "
        "class Item "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Item; "
        "class Main "
        "  public static method Make(): IOwningEnumerable<Item> "
        "    var s := new OwningStack<Item>(); "
        "    s.Push(new Item(1)); "
        "    s.Push(new Item(2)); "
        "    return s; "
        "  end Make; "
        "  public static method Run(): int "
        "    using var e := Make(); "
        "    var order := 0; "
        "    for var x in e do "
        "      using var y := owned x; "
        "      order := order * 10 + y.V; "
        "    end for; "
        "    Console.WriteLine($\"order={order}\"); "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    // Pre-fix on 0c5ecd1 this cell SEGV'd on both compilers: the owning-head
    // drain divert fired inside `wrapStackAsIEnumerator`, and the
    // `freeCollectionSourceNow` that followed it freed the array the drain
    // enumerator had just been handed.
    BEZEL_EXPECT_TRUE(r.ran);
    // stdlib §4.11 pop order: 2 then 1.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "order=21"));
}

// ---------------------------------------------------------------------------
// (E) Verified negative — `Queue<int>` at the same arm keeps dequeue order.
//   per stdlib §4.11 ("a queue front-to-back (dequeue order)")
// ---------------------------------------------------------------------------
BEZEL_TEST(callee_owned_stack_return_wrap_3450,
           callee_owned_queue_return_is_unchanged_4_11) {
    const std::string src =
        "module M; "
        "class Main "
        "  public static method Make(): IEnumerable<int> "
        "    var q := new Queue<int>(); "
        "    q.Enqueue(1); "
        "    q.Enqueue(2); "
        "    q.Enqueue(3); "
        "    return q; "
        "  end Make; "
        "  public static method Run(): int "
        "    using var e := Make(); "
        "    var order := 0; "
        "    for var x in e do "
        "      order := order * 10 + x; "
        "    end for; "
        "    Console.WriteLine($\"order={order}\"); "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    // Identical on both sides of the fix — this arm's non-stack leaf is
    // untouched.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "order=123"));
}

// ---------------------------------------------------------------------------
// (F) Verified negative — `Array<int>` at the same arm.
//   per stdlib §4.4, §28.9
// ---------------------------------------------------------------------------
BEZEL_TEST(callee_owned_stack_return_wrap_3450,
           callee_owned_array_return_is_unchanged_28_9) {
    const std::string src =
        "module M; "
        "class Main "
        "  public static method Make(): IEnumerable<int> "
        "    var a := new Array<int>(); "
        "    a.Add(1); "
        "    a.Add(2); "
        "    a.Add(3); "
        "    return a; "
        "  end Make; "
        "  public static method Run(): int "
        "    using var e := Make(); "
        "    var order := 0; "
        "    for var x in e do "
        "      order := order * 10 + x; "
        "    end for; "
        "    Console.WriteLine($\"order={order}\"); "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "order=123"));
}

// ---------------------------------------------------------------------------
// (G) The element-disposer row — a `Stack<string>` callee-owned return,
//     loop-amplified 200x under LSan.
//   per §28.11 (the element slot policy), §28.9, stdlib §4.11
//
// The strings are INTERPOLATED, not literals: `bz_string_retain` is a no-op on
// a literal/sentinel, so a literal-element program cannot see this leak at all.
// MEASURED on a scratch compiler with this wrap's disposer forced back to
// #2987's hardcoded null: 15200 B leaked in 800 allocations. With the fix: 0.
// ---------------------------------------------------------------------------
BEZEL_TEST(callee_owned_stack_return_wrap_3450,
           callee_owned_string_stack_return_releases_its_elements_28_11) {
    const std::string src =
        "module M; "
        "class Main "
        "  public static method Mk(n: int): string "
        "    return $\"item{n}\"; "
        "  end Mk; "
        "  public static method Make(): IEnumerable<string> "
        "    var s := new Stack<string>(); "
        "    s.Push(Mk(1)); "
        "    s.Push(Mk(2)); "
        "    return s; "
        "  end Make; "
        "  public static method Run(): int "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      using var e := Make(); "
        "      for var x in e do "
        "        total := total + x.Length; "
        "      end for; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine($\"total={total}\"); "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return;
    // `r.ran` false here means LSan reported the retained +1 per element that a
    // null disposer never releases (or an ASan fault from the pre-fix
    // freed-backing walk).
    BEZEL_EXPECT_TRUE(r.ran);
    // 200 iterations x 2 elements x 5 chars ("item1"/"item2").
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=2000"));
}

// ---------------------------------------------------------------------------
// The cache-key programs for rows (H) and (I).
//
// `AaaStr` mints first (the `string` element's release thunk wins the shared
// `…$Dispose`), `ZzzFn` second — so the function-typed elements inherit a
// disposer that calls `bz_string_release` on a pointer into `.text`. Both
// methods return through the SAME callee-owned RETURN arm; only the collection
// family differs between the two programs.
//
// A function type is the partner precisely because it has NO disposal
// obligation: its own correct disposer is null, so nothing else in the program
// leaks or double-frees and the exit status isolates the collision.
// (Never put a C++ `//` comment inside these literals: it is lexed as Bezel
// source and comments out the rest of the program. Bezel's own comment forms
// are `//`, `/* ... */` and `///` -- spec §1.5, which is exhaustive.)
// ---------------------------------------------------------------------------
namespace {

std::string keyCollideProgram(const char* ctor, const char* add) {
    return
        std::string(
            "module M; "
            "class Main "
            "  public static method Mk(n: int): string "
            "    return $\"item{n}\"; "
            "  end Mk; "
            "  public static method Dbl(n: int): int "
            "    return n * 2; "
            "  end Dbl; "
            "  public static method AaaStr(): IEnumerable<string> "
            "    var a := new ") + ctor + "<string>(); " +
            "    a." + add + "(Mk(1)); " +
            "    a." + add + "(Mk(2)); " +
            "    return a; "
            "  end AaaStr; "
            "  public static method ZzzFn(): IEnumerable<(int) -> int> "
            "    var a := new " + ctor + "<(int) -> int>(); " +
            "    a." + add + "(Dbl); " +
            "    a." + add + "(Dbl); " +
            "    return a; "
            "  end ZzzFn; "
            "  public static method Run(): int "
            "    var total := 0; "
            "    using var e1 := AaaStr(); "
            "    for var x in e1 do "
            "      total := total + x.Length; "
            "    end for; "
            "    using var e2 := ZzzFn(); "
            "    for var f in e2 do "
            "      total := total + f(3); "
            "    end for; "
            "    Console.WriteLine($\"total={total}\"); "
            "    return 0; "
            "  end Run; "
            "  public static method Main(): int "
            "    return Run(); "
            "  end Main; "
            "end Main;";
}

}  // namespace

// ---------------------------------------------------------------------------
// (H) The cache-key row, ARRAY family — a live wild free on `origin/main`.
//   per §28.11 (the element slot policy), §28.2 (dispose exactly once)
//
// FAILS on `origin/main`: `total=22` reaches stdout, then the scope-end
// `bz$arrenumown$ptr$Dispose` runs the string release over the function
// elements and the process dies with SIGSEGV (rc 139, 5/5). `r.ran` is
// `WIFEXITED`, so it is false there.
//
// ⚠ `runAot`, not `runAotChecked` — see the header's note on rows (H)/(I).
// ---------------------------------------------------------------------------
BEZEL_TEST(callee_owned_stack_return_wrap_3450,
           owning_array_wrap_disposer_is_keyed_by_element_type_28_11) {
    JitResult r = runAot(keyCollideProgram("Array", "Add"));
    // The crash assertion: WIFEXITED is false when the generated program is
    // killed by SIGSEGV.
    BEZEL_EXPECT_TRUE(r.ran);
    // 5 + 5 (two "itemN") + 6 + 6 (Dbl(3) twice) == 22.
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=22"));
    // Redundant with `r.ran` by construction, and deliberately so: it is the
    // half of the claim that survives if the harness's crash reporting changes.
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// (I) The same shape, STACK family — a FORWARD RATCHET.
//   per §28.11, stdlib §4.11
//
// Unlike (H) this row does not isolate the collision against `origin/main`:
// that arm never reaches the disposer there at all (#3450's own defect — 3/5
// SIGSEGV, 2/5 silent `total=0`). It fails against a key-removed build of THIS
// tree, on which it SEGVs 5/5. It is here so the stack wrap cannot silently
// lose the key segment the array wrap is now pinned on by (H).
// ---------------------------------------------------------------------------
BEZEL_TEST(callee_owned_stack_return_wrap_3450,
           owning_stack_wrap_disposer_is_keyed_by_element_type_28_11) {
    JitResult r = runAot(keyCollideProgram("Stack", "Push"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "total=22"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

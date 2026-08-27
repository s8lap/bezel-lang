// Codegen — issue #3418 (§28.6 leak prevention, §29.2 desugaring of `for`,
// §28.3 local ownership, §28.10 `using var`, spec v0.283): the `using var` twin
// of #3384. A local DECLARED `using var` at an enumerable head and initialized
// from a named collection leaked one §29.2 wrapper frame per execution of the
// declaration, at BOTH heads — byte-identically to the plain-`var` form #3384
// fixed, with only the `using var` keyword differing.
//
// Measured on the pre-fix compiler (`bezelc --sanitize=address`, LSan on):
//
//   using var e: IEnumerable<Item> := a;        over OwningArray  1600 B / 50
//   using var e: IOwningEnumerable<Item> := a;  same source       1200 B / 50
//   using var e: IEnumerable<int> := s;         over Stack<int>   1600 B / 50
//   ... over Set<int>                                             6000 B / 150
//   ... over Array<int>                                           1600 B / 50
//   ... over LinkedList<int>                                      1600 B / 50
//   ... over Queue<int>                                           1600 B / 50
//   a wrap plus a `using var b := e;` alias, one execution           32 B / 1
//
// The plain-`var` spelling of every one of them is clean, before and after.
//
// TWO INDEPENDENT CAUSES, both load-bearing (each was disabled in turn on an
// otherwise-fixed compiler and all three head repros came back at their exact
// pre-fix byte counts):
//
//   1. THE CLAIM PREDICATE. `lowerLocalDecl`'s §29.2 coercion is the WRAP
//      CREATOR for both spellings — `lowerStmt` calls it before branching on
//      `ld->isUsing`. The OWNERSHIP CLAIM is split: the plain `var` goes to
//      `registerIterableSourceDisposal`, which #3384 pointed at the coercion's
//      own `iterFormatWrappedDecls_` self-report; the `using var` goes to
//      `registerUsingDisposal`, which still carried a THIRD, independent
//      spelling of the coercion's admissible (declared head x initializer
//      family) grid — narrower even than the one #3384 replaced, admitting only
//      a head-`Array` initializer or a declaration-carrying one, not even the
//      `Set`/`OwningSet` arm. Every frame minted outside that re-spelling was
//      minted and never claimed, and nothing else frees a wrapper frame (the
//      source collection's own binder disposes the BACKING, not the wrap).
//
//   2. THE §28.10 BORROW-PIN. `registerUsingDisposal` early-returns on
//      `ld->usingVarSourceIsBorrow`, ahead of the arm above, so for a named
//      owner source the registration was skipped BEFORE the predicate was ever
//      consulted. That is why even the `Array` source — which cause (1)'s stale
//      predicate DID admit — leaked 1600 B / 50 pre-fix.
//
// THE FIX, for both, is one authority. #3384's shape transfers unchanged: ask
// the wrap creator what it did rather than re-deriving it. `wrappedHere` becomes
// `iterFormatWrappedDecls_.count(ld)` (#2975's `v != preIterWrap` self-report,
// which `lowerStmt` populates immediately before this registration and CLEARS
// when the coercion declines), and the borrow-pin is lifted on that SAME
// predicate rather than a fourth spelling. #2769 and #2779 each had to add a
// carve-out for one syntactic shape of an iterator-format initializer; the fact
// both were reaching for is simply "did the decl-site coercion mint a frame for
// this declaration", and the coercion answers that itself.
//
// WHY LIFTING THE PIN IS CORRECT, not a widening of §28.10. The pin is about the
// SOURCE object: §28.10 says a borrow-pinned `using var` registers no disposal
// because its real owner disposes it. True of the source; FALSE of the wrapper
// frame, a fresh `calloc` this declaration alone holds and nothing else ever
// frees.
//
// CLAIM SUBSET-OF MINT — the dangerous direction, since claiming an unminted
// frame is a double-free, strictly worse than the leak. The report has exactly
// ONE writer, under the coercion's own observation, and is never widened by a
// second predicate; the rewrite is a strict SUPERSET of the old positive list,
// so no existing claim is lost. Every helper the coercion can call frees only
// the frame it minted — the borrowing wraps free `self`, and
// `wrapArrayAsIOwningEnumerator` frees `self` plus an advanced-but-untaken
// pending it owns per §29.1 — never the source backing. The lift also cannot
// reach any OTHER disposal arm: the coercion's own gate requires an explicit
// declared type whose head is `IEnumerable` or `IOwningEnumerable`, so
// membership in the report implies the `isSyncIterEnum` arm matches and returns,
// which is exactly what the #2769 / #2779 carve-outs needed an explicit
// iterator-format head test to guarantee. Asserted here under generated-code
// ASan+LSan, which aborts on a double free or use-after-free: the source
// collection is intact and its values are still readable across 40 wrap-and-
// iterate cycles, and a wrap plus a bare `using var` ALIAS of it disposes the
// frame exactly once.
//
// Every test uses runAotChecked (ASan with LeakSanitizer on), so `r.ran` is
// itself the leak assertion; each returns 9 through `Main` so a silent
// mis-execution cannot pass. Each `using var` test is paired with its plain-`var`
// twin, which was clean before and must stay clean.
//
// FAIL-WITHOUT / PASS-WITH, measured program-by-program on one build of each
// compiler (the only difference being this fix's two hunks reverted):
//
//   program                                       pre-fix          post-fix
//   using var e: IEnumerable<Item> := a   x50     1600 B / 50      clean
//   var       e: IEnumerable<Item> := a   (ctrl)  clean            clean
//   using var e: IOwningEnumerable<Item>  x50     1200 B / 50      clean
//   var       e: IOwningEnumerable<Item>  (ctrl)  clean            clean
//   using var e: IEnumerable<int> := stack x50    1600 B / 50      clean
//   var       e: IEnumerable<int> := stack (ctrl) clean            clean
//   ... := set                             x50    6000 B / 150     clean
//   ... := array                           x50    1600 B / 50      clean
//   ... := linked list                     x50    1600 B / 50      clean
//   ... := queue                           x50    1600 B / 50      clean
//   wrap + iterate, values intact          x40    1280 B / 40      clean
//   wrap + bare `using var` alias                 32 B / 1         clean
//   a user class source (control)                 clean            clean
// per §28.6, §29.2, §28.3, §28.10, §29.1 (issue #3418)

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
// The program's exit code, or a distinctive sentinel when the checked run did
// not complete cleanly (a leak, a use-after-free, a double free) so a failure
// diff names the oracle rather than a bare 0.
int runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}

// A named `using var` OwningArray<Item> source, iterated 50 times through a
// `using var` local declared at HEAD. `%HEAD%` and `%KW%` are substituted.
std::string wrapLoop(const std::string& kw, const std::string& head) {
    return
        "module M; "
        "class Item "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Item; "
        "class Main "
        "  public static method Run(): int "
        "    using var a := new OwningArray<Item>(); "
        "    a.Add(new Item(9)); "
        "    var i := 0; "
        "    while i < 50 do "
        "      " + kw + " e: " + head + "<Item> := a; "
        "      i := i + 1; "
        "    end while; "
        "    return 9; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
}

// The same loop over a named borrowing collection of `int`, at the
// `IEnumerable<int>` head. `ctor` / `add` name the source's construction.
std::string srcLoop(const std::string& kw, const std::string& ctor,
                    const std::string& add) {
    return
        "module M; "
        "class Main "
        "  public static method Run(): int "
        "    using var s := " + ctor + "; "
        "    " + add + " "
        "    var i := 0; "
        "    while i < 50 do "
        "      " + kw + " e: IEnumerable<int> := s; "
        "      i := i + 1; "
        "    end while; "
        "    return 9; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;";
}
}  // namespace

// ── THE ISSUE'S FIRST REPRO: the `IEnumerable<T>` head over a named
// `OwningArray<T>`. 1600 bytes in 50 allocations pre-fix (32 B per
// `{vtable, arr, idx, esz}` borrowing frame).
// per §28.6, §29.2, §28.10 (issue #3418)
BEZEL_TEST(using_var_enumerable_wrapper_claim_3418,
           using_var_ienumerable_head_over_owning_array_spec_28_6_29_2) {
    BEZEL_EXPECT_EQ(runChecked(wrapLoop("using var", "IEnumerable")), 9);
}

// ── PARITY CONTROL: the plain-`var` spelling, fixed by #3384, must stay clean.
// per §28.6, §29.2 (issue #3418, #3384)
BEZEL_TEST(using_var_enumerable_wrapper_claim_3418,
           var_ienumerable_head_over_owning_array_parity_spec_28_6) {
    BEZEL_EXPECT_EQ(runChecked(wrapLoop("var", "IEnumerable")), 9);
}

// ── THE SECOND HEAD: `IOwningEnumerable<T>` over the same source mints the
// 24-byte `{vtable, arr, pending}` DRAINING frame instead. 1200 B / 50 pre-fix.
// Its Dispose frees `self` plus an advanced-but-untaken pending it owns per
// §29.1 — never the source backing, which is why claiming it cannot double-free
// the named owner. // per §28.6, §29.1, §29.2, §28.10 (issue #3418)
BEZEL_TEST(using_var_enumerable_wrapper_claim_3418,
           using_var_iowningenumerable_head_over_owning_array_spec_29_1) {
    BEZEL_EXPECT_EQ(runChecked(wrapLoop("using var", "IOwningEnumerable")), 9);
}

// ── PARITY CONTROL for the owning head.
// per §28.6, §29.1 (issue #3418, #3384)
BEZEL_TEST(using_var_enumerable_wrapper_claim_3418,
           var_iowningenumerable_head_over_owning_array_parity_spec_29_1) {
    BEZEL_EXPECT_EQ(runChecked(wrapLoop("var", "IOwningEnumerable")), 9);
}

// ── SOURCE-FAMILY axis, `Stack<int>` (#1177's bz_array_t-backed family). The
// stale claim predicate admitted neither this nor any family below. 1600 B / 50.
// per §28.6, §29.2, stdlib §4.11 (issue #3418)
BEZEL_TEST(using_var_enumerable_wrapper_claim_3418,
           using_var_ienumerable_head_over_stack_spec_28_6_29_2) {
    BEZEL_EXPECT_EQ(runChecked(srcLoop(
        "using var", "new Stack<int>()", "s.Push(9);")), 9);
}

BEZEL_TEST(using_var_enumerable_wrapper_claim_3418,
           var_ienumerable_head_over_stack_parity_spec_28_6) {
    BEZEL_EXPECT_EQ(runChecked(srcLoop(
        "var", "new Stack<int>()", "s.Push(9);")), 9);
}

// ── SOURCE-FAMILY axis, `Set<int>` (#1069). 6000 B / 150 pre-fix — the
// dict-backed set is snapshotted, so its wrap costs three allocations, not one.
// per §28.6, §29.2, stdlib §4.10 (issue #3418)
BEZEL_TEST(using_var_enumerable_wrapper_claim_3418,
           using_var_ienumerable_head_over_set_spec_28_6_29_2) {
    BEZEL_EXPECT_EQ(runChecked(srcLoop(
        "using var", "new Set<int>()", "s.Add(9);")), 9);
}

// ── SOURCE-FAMILY axis, `Array<int>` — THE PIN ON CAUSE (2). This is the ONE
// family the stale claim predicate did admit (`initTR->headName() == "Array"`),
// so it isolates the §28.10 borrow-pin: with only the predicate repaired it
// still leaked 1600 B / 50, because the early return fired before the predicate
// was ever consulted. // per §28.6, §28.10, §29.2 (issue #3418)
BEZEL_TEST(using_var_enumerable_wrapper_claim_3418,
           using_var_ienumerable_head_over_array_borrow_pin_spec_28_10) {
    BEZEL_EXPECT_EQ(runChecked(srcLoop(
        "using var", "new Array<int>()", "s.Add(9);")), 9);
}

// ── SOURCE-FAMILY axis, `LinkedList<int>` (#1177's node-walking wrapper).
// per §28.6, §29.2, stdlib §4.11 (issue #3418)
BEZEL_TEST(using_var_enumerable_wrapper_claim_3418,
           using_var_ienumerable_head_over_linked_list_spec_28_6_29_2) {
    BEZEL_EXPECT_EQ(runChecked(srcLoop(
        "using var", "new LinkedList<int>()", "s.AddLast(9);")), 9);
}

// ── SOURCE-FAMILY axis, `Queue<int>`.
// per §28.6, §29.2, stdlib §4.11 (issue #3418)
BEZEL_TEST(using_var_enumerable_wrapper_claim_3418,
           using_var_ienumerable_head_over_queue_spec_28_6_29_2) {
    BEZEL_EXPECT_EQ(runChecked(srcLoop(
        "using var", "new Queue<int>()", "s.Enqueue(9);")), 9);
}

// ── THE NO-DOUBLE-FREE ASSERTION. 40 wrap-and-iterate cycles over ONE named
// `using var` OwningArray: the wrapper frame is claimed and freed each time
// while the source and its element stay intact and readable (the total pins
// that), and the source's own single §28.3 disposal still runs exactly once at
// its scope end. Under ASan a claim over the source backing would abort as a
// double free or use-after-free rather than merely leaking. 1280 B / 40 pre-fix.
// per §28.3, §28.6, §29.2 (issue #3418)
BEZEL_TEST(using_var_enumerable_wrapper_claim_3418,
           wrapped_source_intact_and_disposed_once_spec_28_3_28_6) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class Item "
        "  public V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Item; "
        "class Main "
        "  public static method Run(): int "
        "    using var a := new OwningArray<Item>(); "
        "    a.Add(new Item(9)); "
        "    var total := 0; "
        "    var i := 0; "
        "    while i < 40 do "
        "      using var e: IEnumerable<Item> := a; "
        "      for var it in e do "
        "        total := total + it.V; "
        "      end for; "
        "      i := i + 1; "
        "    end while; "
        "    if total <> 360 then return 1; end if; "
        "    return 9; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;"), 9);
}

// ── THE ALIAS ASSERTION, the other half of claim-subset-of-mint: a `using var`
// that WRAPS (`e`) followed by a bare `using var` ALIAS of it (`b`). Only `e`'s
// declaration minted a frame, so only `e` may claim it; `b` mints nothing and
// must register nothing, or the single frame would be disposed twice. Pre-fix
// `e`'s frame leaked (32 B / 1) and `b` claimed nothing either; post-fix the
// frame is freed exactly once and the iteration through `b` still sees the
// values. // per §28.3, §28.6, §29.2, §28.10 (issue #3418)
BEZEL_TEST(using_var_enumerable_wrapper_claim_3418,
           wrap_then_bare_alias_disposes_frame_exactly_once_spec_28_3) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class Main "
        "  public static method Run(): int "
        "    using var a := new Array<int>(); "
        "    a.Add(3); "
        "    a.Add(4); "
        "    using var e: IEnumerable<int> := a; "
        "    using var b := e; "
        "    var t := 0; "
        "    for var x in b do "
        "      t := t + x; "
        "    end for; "
        "    if t <> 7 then return 1; end if; "
        "    return 9; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;"), 9);
}

// ── CONTROL (clean before AND after): a USER CLASS source (§29.2 / #552). The
// coercion's `wrapUserClassAsIEnumerable` has five DECLINE paths and hands the
// value back unchanged on each; the report is CLEARED then, so the claim
// declines with it. This shape was already clean, and pins that reading the
// report rather than the static types did not start claiming a frame that was
// never minted. // per §29.2, §28.6 (issue #3418, #2975)
BEZEL_TEST(using_var_enumerable_wrapper_claim_3418,
           using_var_ienumerable_head_over_user_class_control_spec_29_2) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; "
        "class MyRange: IEnumerable<int> "
        "  private _n: int; "
        "  public constructor(n: int) _n := n; end constructor; "
        "  public method GetEnumerator(): owned IEnumerator<int> "
        "    for var i in 0.._n do "
        "      yield return i; "
        "    end for; "
        "  end GetEnumerator; "
        "end MyRange; "
        "class Main "
        "  public static method Run(): int "
        "    var i := 0; "
        "    while i < 50 do "
        "      using var src := new MyRange(3); "
        "      using var e: IEnumerable<int> := src; "
        "      i := i + 1; "
        "    end while; "
        "    return 9; "
        "  end Run; "
        "  public static method Main(): int "
        "    return Run(); "
        "  end Main; "
        "end Main;"), 9);
}

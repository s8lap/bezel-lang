// Codegen — issue #3375 (§21.1 runtime `const` locals, §29.3 iterator methods,
// §30.1/§30.2 `async`/`await`, §28.9 runtime-managed types, §15.4 collection
// literals; spec v0.282): a `const` local declared in a state-machine body
// (an `async` method or an iterator method) was given NO state-machine frame
// field, so its value did not survive a suspension point.
//
// THE DEFECT. §21.1's "Runtime `const` locals" makes a `const` local a
// SINGLE-ASSIGNMENT LOCAL BINDING: "initialized exactly once, at its
// declaration, by ordinary runtime evaluation", readable for the rest of its
// scope. It differs from a `var` in exactly one respect — it is not an
// assignable location. §30.1 says an async body's locals whose live range
// crosses a suspension point are "captured into the state-machine frame", and
// §29.3 says an iterator body is likewise "translate[d] ... into the
// equivalent state-machine implementation of `IEnumerator<T>`", with §29.4
// applying the same across-suspension frame rule to `yield return`.
//
// `collectSuspendLocals` (src/codegen/LlvmBackend_Iterators.cpp) is the ONE
// authority that decides which body locals get a frame field: everything it
// pushes into `bodyLocals` becomes a frame slot, lands in `smLocalFields_`,
// and is spilled/restored around every suspend by `emitAwaitSuspend`'s
// `smAllocaToField_` walk (async) and the yield handler (iterators). It
// matched `ast::LocalDecl` — and ONLY `ast::LocalDecl`. §21.1's `const` is a
// different AST node, `ast::LocalConstDecl`, so no `const` local ever got a
// frame field. `lowerLocalDecl` (which lowers BOTH node kinds — see
// LlvmBackend_Stmts.cpp's LocalConstDecl arm) then found nothing in
// `smLocalFields_` for it and gave it a bare entry alloca, which lives only in
// the CURRENT activation of the resume / MoveNext function. After a suspend
// the reload therefore read a fresh, uninitialised stack slot.
//
// This is the same shape as, and the root cause of, the leak deferred out of
// #2802: the §28.9 scope-end reclaim for a `const d: Dictionary<K,V> := [...]`
// literal IS registered and IS emitted, but it loads a null/garbage slot, so
// its null guard skips `bz_dict_free` and the table leaks. The leak was only
// the mildest symptom. Measured on the pre-fix compiler:
//
//   const d: Dictionary<string,int> := [...]; await ...; return d.Count;
//        -> LeakSanitizer: 420 byte(s) leaked in 5 allocation(s)
//   const a: int := 41;   await ...; use a    -> a read back as 31617
//   const s: string := "hello"; await ...; use s -> SEGV in bz$async$Main$Resume
//   const Set/Array literals across an await   -> SEGV in bz_dict_count
//   const dict+set literals in an awaiting loop-> ASan bad-free (wild pointer)
//   const locals in a sync iterator            -> wrong values (32777 vs 17)
//
// so the defect class is memory-unsafety, not merely a leak.
//
// THE FIX adds the `ast::LocalConstDecl` arm to `collectSuspendLocals` (and to
// its one-liner `catch E => ...` arm sibling), deriving the frame slot type by
// the SAME rule the `LocalDecl` arm uses — the rule was hoisted into one
// `frameSlotTy` lambda so there is no second copy of it, including #461's
// pointer-width default and #808's ptr widening for a lambda-captured local.
// Nothing else changes: `lowerLocalDecl`'s frame-backing path already keys on
// the generic `declNode`, and the spill/restore walks are generic over
// `smAllocaToField_` / `frame.localFields`.
//
// WHY IT IS NOT A DOUBLE-FREE (Appendix C). The fix adds no disposal. It
// makes an EXISTING, already-registered disposal load the value it was always
// meant to load: the reclaim is still emitted exactly once, at the one scope
// end, against the one slot, under its existing null guard. A `const` is by
// §21.1 never an owned production and never takes a disposal obligation, so
// no ownership discharge is added or moved; the spill/restore is a pure
// value round-trip through a frame field that no other code frees.
//
// Every test below uses runAotChecked (generated-code ASan with LeakSanitizer
// on), so `r.ran` is itself the leak assertion. Each FAILS on the pre-fix
// compiler (leak, SEGV, bad-free, or wrong value) and PASSES with the fix.
// per §21.1, §29.3, §30.1, §30.2, §28.9, §15.4 (issue #3375)

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {
int runChecked(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.exitCode : -999;
}
std::string runCheckedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
}  // namespace

// ── The issue's exact repro: a §21.1 `const` Dictionary collection literal
// held across an `await`. Pre-fix: 420 bytes / 5 allocations leaked
// (bz_dict_alloc x2, dict_order_append, strndup x2) because the §28.9
// scope-end reclaim loaded a stale slot. Value: 2.
// per §21.1, §30.2, §28.9, §15.4 (issue #3375)
BEZEL_TEST(async_const_local_frame_spill_3375,
           const_dict_literal_across_await_reclaimed_spec_21_1_30_2_28_9) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    const d: Dictionary<string, int> := [\"a\": 10, \"b\": 20]; "
        "    await Task.Delay(30); "
        "    return d.Count; "
        "  end Main; "
        "end C;"),
        2);
}

// ── VALUE axis, primitive: the leak was the mild symptom; the general defect
// is that a `const` local simply did not survive the suspend. §21.1 makes the
// binding readable for the rest of its scope. Pre-fix this returned stack
// garbage (measured: 31617 for the constant 41).
// per §21.1, §30.2 (issue #3375)
BEZEL_TEST(async_const_local_frame_spill_3375,
           const_int_local_survives_await_spec_21_1_30_2) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    const a: int := 41; "
        "    await Task.Delay(30); "
        "    return a; "
        "  end Main; "
        "end C;"),
        41);
}

// ── VALUE axis, reference (string): pre-fix this SEGV'd in
// bz$async$Main$Resume — the slot held a garbage pointer that was then
// dereferenced. Observable goes through stdout (a string is not an exit code).
// per §21.1, §30.2, §28.9 (issue #3375)
BEZEL_TEST(async_const_local_frame_spill_3375,
           const_string_local_survives_await_spec_21_1_30_2_28_9) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    const s: string := \"hello\"; "
        "    await Task.Delay(30); "
        "    Console.WriteLine(\"s=\" + s); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "s=hello\n");
}

// ── BREADTH axis: every §15.4 literal family plus the value-typed and
// nullable const forms in one body, so a fix that special-cased Dictionary
// would not pass. Pre-fix: SEGV in bz_dict_count. Value: 3 + 4 + 2 = 9.
// per §21.1, §15.4, §30.2, §28.9 (issue #3375)
BEZEL_TEST(async_const_local_frame_spill_3375,
           const_set_array_dict_and_scalars_across_await_spec_15_4_21_1) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    const st: Set<string> := [\"a\", \"b\", \"c\"]; "
        "    const arr: Array<int> := [1, 2, 3, 4]; "
        "    const d: Dictionary<string, int> := [\"a\": 10, \"b\": 20]; "
        "    const n: int? := 7; "
        "    const dbl: double := 2.5; "
        "    const b: bool := true; "
        "    await Task.Delay(20); "
        "    Console.WriteLine(\"st=\" + st.Count.ToString()); "
        "    Console.WriteLine(\"arr=\" + arr.Count.ToString()); "
        "    Console.WriteLine(\"d=\" + d.Count.ToString()); "
        "    Console.WriteLine(\"n=\" + n!.ToString()); "
        "    Console.WriteLine(\"dbl=\" + dbl.ToString()); "
        "    Console.WriteLine(\"b=\" + b.ToString()); "
        "    return st.Count + arr.Count + d.Count; "
        "  end Main; "
        "end C;"),
        "st=3\narr=4\nd=2\nn=7\ndbl=2.5\nb=True\n");
}

// ── LOOP axis: `const` collection literals declared inside a loop body that
// also suspends. Pre-fix this was an ASan **bad-free** (the stale slot held a
// wild pointer that the §28.9 reclaim then passed to free) — the sharpest
// evidence that this is a memory-safety defect, not just a leak.
// 500 iterations * (2 + 3) = 2500, reported on stdout (an exit code is 8-bit).
// per §21.1, §15.4, §28.9, §30.2 (issue #3375)
BEZEL_TEST(async_const_local_frame_spill_3375,
           const_literals_in_awaiting_loop_no_bad_free_spec_28_9_30_2) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static async method Loop(): Task<int> "
        "    var total := 0; "
        "    for var i in 1..500 do "
        "      const d: Dictionary<int, int> := [1: 10, 2: 20]; "
        "      const s: Set<int> := [1, 2, 3]; "
        "      total := total + d.Count + s.Count; "
        "      if i mod 100 = 0 then "
        "        await Task.Delay(1); "
        "      end; "
        "    end; "
        "    return total; "
        "  end Loop; "
        "  public static async method Main(): Task<int> "
        "    const t := await Loop(); "
        "    Console.WriteLine(\"total=\" + t.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "total=2500\n");
}

// ── TRY-REGION axis: a `const` declared in a `try` body whose live range
// crosses an in-region `await` (§30.9/#438 recurses collectSuspendLocals into
// try regions — that recursion reached only LocalDecl before this fix).
// Pre-fix: SEGV in bz_dict_count. Value: 3.
// per §21.1, §30.2, §28.9 (issue #3375)
BEZEL_TEST(async_const_local_frame_spill_3375,
           const_dict_literal_in_try_body_across_await_spec_21_1_30_2) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var r := 0; "
        "    try "
        "      const d: Dictionary<string, int> := [\"a\": 1, \"b\": 2, \"c\": 3]; "
        "      await Task.Delay(20); "
        "      r := d.Count; "
        "    catch "
        "      Exception => do r := -1; end "
        "    end; "
        "    return r; "
        "  end Main; "
        "end C;"),
        3);
}

// ── ITERATOR axis: `collectSuspendLocals` serves BOTH state machines, so a
// sync iterator's `const` locals were equally unbacked across `yield return`
// (§29.3 / §29.4). The consumer calls a stack-churning helper between
// MoveNext() calls so the stale allocas are provably clobbered rather than
// accidentally intact. Gen yields 5, 2, 7; Churn always returns 1; so
// t = ((0+5+1)+2+1)+7+1 = 17. Pre-fix this printed t=32777.
// per §21.1, §29.3, §29.4 (issue #3375)
BEZEL_TEST(async_const_local_frame_spill_3375,
           const_locals_survive_yield_return_in_iterator_spec_29_3_29_4) {
    BEZEL_EXPECT_EQ(runCheckedOut(
        "module M; import System; "
        "class C "
        "  public static method Gen(): IEnumerable<int> "
        "    const a: int := 5; "
        "    const d: Dictionary<string, int> := [\"k\": 9, \"j\": 8]; "
        "    yield return a; "
        "    yield return d.Count; "
        "    yield return a + d.Count; "
        "  end Gen; "
        "  public static method Churn(n: int): int "
        "    var acc := 0; "
        "    var m: Dictionary<string, int> := [\"p\": n, \"q\": n + 1, \"r\": n + 2]; "
        "    for var i in 1..40 do "
        "      acc := acc + m[\"p\"] + m[\"q\"] + m[\"r\"] + i; "
        "    end; "
        "    return acc mod 3; "
        "  end Churn; "
        "  public static method Main(): int "
        "    var t := 0; "
        "    for var v in Gen() do "
        "      t := t + v + Churn(t); "
        "    end; "
        "    Console.WriteLine(\"t=\" + t.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "t=17\n");
}

// ── REGRESSION: the `var` form (already frame-backed before this fix) must
// stay correct and ASan-clean — the fix must widen the frame set, not perturb
// the existing membership. Value: 2.
// per §30.2, §28.9, §15.4 (issue #3375; regression-vs-#2143/#2802)
BEZEL_TEST(async_const_local_frame_spill_3375,
           var_dict_literal_across_await_unchanged_regression_spec_30_2_28_9) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var d: Dictionary<string, int> := [\"a\": 10, \"b\": 20]; "
        "    await Task.Delay(30); "
        "    return d.Count; "
        "  end Main; "
        "end C;"),
        2);
}

// ── REGRESSION: the SYNC `const` path #2802 fixed must stay clean (no
// suspension involved — this exercises that the widened frame set is inert
// outside a state machine, where smStateStruct_ is null). Value: 2.
// per §21.1, §28.9, §15.4 (issue #3375; regression-vs-#2802)
BEZEL_TEST(async_const_local_frame_spill_3375,
           const_dict_literal_sync_unchanged_regression_spec_21_1_28_9) {
    BEZEL_EXPECT_EQ(runChecked(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    const d: Dictionary<string, int> := [\"a\": 10, \"b\": 20]; "
        "    return d.Count; "
        "  end Main; "
        "end C;"),
        2);
}

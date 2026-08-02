// fix #2356 / §29.3 "Iterator methods" + §29.8 "Iterators with owned
// parameters" + §29.11 "Multiple enumeration" (docs v0.234):
//
//   §29.3: "An async iterator method (one declared `async` that uses
//   `yield`; `await` is permitted but not required) has return type
//   `IAsyncEnumerable<T>` (re-iterable) or `IAsyncEnumerator<T>`
//   (single-use) ... `owned` parameters remain admitted (§29.8)."
//
//   §29.8: "An iterator method may take `owned` parameters. The state
//   machine takes ownership at construction time. When the iterator is
//   disposed, the state machine disposes any `owned` parameter that has
//   not been moved out of by the body."
//
//   §30.7's interface block: "GetAsyncEnumerator(token: CancellationToken
//   := CancellationToken.None): owned IAsyncEnumerator<T>" — a FRESH,
//   caller-owned enumerator per call.
//
//   §29.11: "Each `GetEnumerator()` call produces an independent
//   enumerator." (Stated for the sync protocol; §29.3 extends the same
//   re-iterable contract to `IAsyncEnumerable<T>`.)
//
// VERIFY-FIRST finding (root cause, confirmed via scratch-copy revert +
// ASan): for an async iterator with an `owned` parameter, vtable slot 4
// (GetAsyncEnumerator, LlvmBackend_Async.cpp's `hasOwnedParamCapture`
// branch) returns `self` UNCHANGED rather than an independent clone — the
// affine owned parameter (§29.8) cannot be duplicated across two frames, so
// the enumerABLE and the enumerATOR are literally the same heap object.
// `await for`'s own desugaring already special-cases this identity
// (`afor.enown`, LlvmBackend_Stmts.cpp, since #1268) so a bound loop source
// is left undisposed by the loop itself. But a HAND-WRITTEN
// `await using var en := src.GetAsyncEnumerator();` on a NAMED `src` local
// had no equivalent guard: `en`'s own `await using` scope-end disposal AND
// `src`'s separately-registered named-local scope-end disposal both fired
// on the SAME object — heap-use-after-free (confirmed class, matching the
// issue's own ASan capture: freed by `bz$asynciter$UseCell$Dispose`,
// UAF-read in `bz$async$Main$Resume`).
//
// §29.3/§29.11 reading (THE central question, settled from the text): does
// self-return violate the "independent enumerator" contract, or is it a
// permitted consequence of §29.8's affine owned-parameter wording? §29.3
// states the SAME defining clause for both forms — "`IEnumerable<T>`
// (re-iterable: each call to `GetEnumerator()` runs the body again from the
// start)" and, immediately after, "an async iterator method ... has return
// type `IAsyncEnumerable<T>` (re-iterable) ... `owned` parameters remain
// admitted (§29.8)" — with NO carve-out anywhere in §29.3, §29.8, §29.11, or
// §30.7 for the owned-parameter shape. "Remain admitted" reads as "this
// parameter kind does not disqualify the method from being an iterator at
// all" (the sentence's other half forbids `ref`/`out` outright), not as "and
// re-iterability is waived." §30.7's own interface types the method
// `GetAsyncEnumerator(...): owned IAsyncEnumerator<T>`, and §29.1 glosses
// `owned`-returning as "the caller takes ownership ... and must dispose it"
// — a fresh mint per call (§28.3's producer convention), which `self` is
// not: the ENUMERABLE's own binder already owns that object. So self-return
// is read here as spec-NON-compliant — the double-free #2356 reported was
// the SYMPTOM (two valid owners of one heap object, §28's "disposed exactly
// once" broken at the root), not an independent bookkeeping slip to paper
// over with a disposal guard alone.
//
// THE FIX has two layers, both landed:
//
//  1. (Async.cpp, `emitAsyncIteratorMethod`'s GetAsyncEnumerator body, vtable
//     slot 4.) Root-cause: GetAsyncEnumerator now ALWAYS hands back a
//     genuine independent clone (the same byte-copy + state-reset machinery
//     already used for the no-owned-param case, #1268) — never `self`. The
//     affine `owned` parameter cannot be duplicated, so the FIRST call
//     CLAIMS every owned-param capture (the clone's copy stays live; `self`'s
//     own copy is nulled, making its later Dispose a no-op for it via the
//     SAME null-guard the moved-from case already relies on) and marks
//     `self` claimed (state sentinel `-3`); a SECOND GetAsyncEnumerator call
//     on the same source raises `InvalidOperationException` rather than
//     handing out a second alias — mirroring the sync iterator's own
//     #973a/#2154 GetEnumerator claim (LlvmBackend_Iterators.cpp), simplified
//     to claim EVERY owned param uniformly on the first call rather than
//     replicating that path's finer-grained "only claim if actually
//     consumed" sharing (tracked as a follow-up, #2377, for whoever wants
//     the extra throughput of letting a never-consumed capture be safely
//     read from multiple independent clones the way the sync path allows).
//     This eliminates the aliasing at its ROOT for the compiler's own
//     generated owned-param async iterators — `en` can no longer equal
//     `src` for this producer.
//
//  2. (Setup.cpp, `registerUsingDisposal`'s `isAwaitUsing` branch.) Defense
//     in depth, KEPT (not dead code): `IAsyncEnumerable<T>` is a plain
//     interface — an arbitrary USER type may implement it (and
//     `IAsyncEnumerator<T>` on the very same class) with a hand-written
//     `GetAsyncEnumerator()` that returns `this`, entirely legally; the
//     compiler cannot rule that out or rewrite user code. For THAT case —
//     genuinely outside this fix's reach — a hand-written
//     `await using var en := src.GetAsyncEnumerator();` on a named `src`
//     would still alias, so the runtime identity guard (mirroring `await
//     for`'s pre-existing `afor.enown` select) stays in place as the general
//     safety net for arbitrary producers.
//
// FAILS WITHOUT / PASSES WITH: `named_source_owned_param_no_crash` below
// (the issue's exact repro) — heap-use-after-free under ASan without EITHER
// fix (mechanically confirmed per-layer: scratch-copy diff to a patch, `git
// checkout` revert, rebuild, reproduce, `git apply` reapply, rebuild,
// confirm clean — see the fix commit for both patches' isolated before/after
// runs).

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan oracle
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
int run(const std::string& src) {
    JitResult r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// §29.8 + §29.11: the issue's EXACT repro. `src` is a named plain `var`
// holding the owned-param async iterable; `en` is a named `await using var`
// obtained by calling `.GetAsyncEnumerator()` directly on `src` (the
// hand-written manual-protocol spelling, not `await for`). Before the fix:
// ASan heap-use-after-free (`bz$async$Main$Resume` reading through the
// frame `bz$asynciter$UseCell$Dispose` already freed). After the fix: clean
// exit, m1 observes the single yielded value (7 -> true -> return 1).
BEZEL_TEST(async_get_async_enumerator_owned_param_alias_2356,
           named_source_owned_param_no_crash) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Cell public V: int := 7; "
        "  public constructor() end constructor; "
        "end Cell; "
        "class P "
        "  public static async method UseCell(owned c: Cell): IAsyncEnumerable<int> "
        "    yield return c.V; "
        "  end UseCell; "
        "  public static async method Main(): Task<int> "
        "    var src := P.UseCell(new Cell()); "
        "    await using var en := src.GetAsyncEnumerator(); "
        "    var m1 := await en.MoveNextAsync(); "
        "    if m1 then return 1; else return 2; end; "
        "  end Main; "
        "end P;"), 1);
}

// Same shape, but Cell.n counts live instances (constructor +1, destructor
// -1) — proves the owned parameter is disposed EXACTLY ONCE (not zero, not
// twice) once `src`'s own scope ends. Cell.n == 0 after Main returns would
// be masked by the return-value check alone; asserting it explicitly closes
// that gap.
BEZEL_TEST(async_get_async_enumerator_owned_param_alias_2356,
           named_source_owned_param_disposed_exactly_once) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Cell public static n: int := 0; public V: int := 7; "
        "  public constructor() Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "class P "
        "  public static async method UseCell(owned c: Cell): IAsyncEnumerable<int> "
        "    yield return c.V; "
        "  end UseCell; "
        "  public static async method Main(): Task<int> "
        "    var v: int := 0; "
        "    do "
        "      var src := P.UseCell(new Cell()); "
        "      await using var en := src.GetAsyncEnumerator(); "
        "      var m1 := await en.MoveNextAsync(); "
        "      if m1 then v := en.Current; end; "
        "    end; "                          // src's scope ends here
        "    if Cell.n <> 0 then return 1; end; "  // disposed exactly once
        "    return v; "                      // 7
        "  end Main; "
        "end P;"), 7);
}

// Adversarial self-pass (a): exactly-once disposal on the ABANDONED path
// too — the iterator yields THREE values but `en`'s scope ends after only
// the first `MoveNextAsync()` (early abandonment, §29.7). Cell.n back to 0
// confirms the owned param's single disposal fires via the source's own
// scope-end teardown (which also runs the iterator's pending state, §29.7),
// not zero (leak) and not twice (double free — ASan would abort).
BEZEL_TEST(async_get_async_enumerator_owned_param_alias_2356,
           named_source_owned_param_early_abandonment_disposed_once) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Cell public static n: int := 0; public V: int := 7; "
        "  public constructor() Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "class P "
        "  public static async method UseCell(owned c: Cell): IAsyncEnumerable<int> "
        "    yield return c.V; "
        "    yield return c.V + 1; "
        "    yield return c.V + 2; "
        "  end UseCell; "
        "  public static async method Main(): Task<int> "
        "    var v: int := 0; "
        "    do "
        "      var src := P.UseCell(new Cell()); "
        "      await using var en := src.GetAsyncEnumerator(); "
        "      var m1 := await en.MoveNextAsync(); "  // only ONE of three
        "      if m1 then v := en.Current; end; "
        "    end; "
        "    if Cell.n <> 0 then return 1; end; "
        "    return v; "                      // 7
        "  end Main; "
        "end P;"), 7);
}

// Adversarial self-pass (c): the owned-param source is declared and NEVER
// enumerated at all (no `GetAsyncEnumerator()` call reaches
// registerUsingDisposal's new guard). `src`'s own scope-end disposal
// (registerIterableSourceDisposal — untouched by this fix) must still
// dispose the owned Cell exactly once.
BEZEL_TEST(async_get_async_enumerator_owned_param_alias_2356,
           named_source_owned_param_never_enumerated_disposed_once) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Cell public static n: int := 0; public V: int := 7; "
        "  public constructor() Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "class P "
        "  public static async method UseCell(owned c: Cell): IAsyncEnumerable<int> "
        "    yield return c.V; "
        "  end UseCell; "
        "  public static async method Main(): Task<int> "
        "    do "
        "      var src := P.UseCell(new Cell()); "
        "    end; "                           // never enumerated
        "    return Cell.n; "                 // 0: disposed once, not leaked
        "  end Main; "
        "end P;"), 0);
}

// The documented WORKAROUND (issue's own "Workaround" section, and the
// shape #1525's own lane used to route around this bug): an UNBOUND
// temporary receiver — `P.UseCell(new Cell()).GetAsyncEnumerator()` — has
// no competing named-local disposal to alias against. This must keep
// working exactly as before (the direct-call site's own existing
// unbound-receiver guard, LlvmBackend_Calls2.cpp, is untouched by this
// fix). Cell.n == 0 confirms the owned param is disposed exactly once
// through the self-returned frame's own disposal (which `en`'s using-var
// now owns, since there is no separate `src` binder).
BEZEL_TEST(async_get_async_enumerator_owned_param_alias_2356,
           unbound_temporary_receiver_workaround_still_works) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Cell public static n: int := 0; public V: int := 7; "
        "  public constructor() Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "class P "
        "  public static async method UseCell(owned c: Cell): IAsyncEnumerable<int> "
        "    yield return c.V; "
        "  end UseCell; "
        "  public static async method Main(): Task<int> "
        "    var v: int := 0; "
        "    do "
        "      await using var en := P.UseCell(new Cell()).GetAsyncEnumerator(); "
        "      var m1 := await en.MoveNextAsync(); "
        "      if m1 then v := en.Current; end; "
        "    end; "
        "    if Cell.n <> 0 then return 1; end; "
        "    return v; "                      // 7
        "  end Main; "
        "end P;"), 7);
}

// §29.11 re-iterability control, NON-owned-param: this fix must not regress
// the genuinely-independent clone path (#1268). Two SEPARATE manual
// `.GetAsyncEnumerator()` calls on one named `IAsyncEnumerable<T>` source
// with no owned parameter each get a FRESH clone (`en != src` at runtime),
// so my new guard's select must be a no-op here — both `en1`/`en2` and
// `src` are disposed independently, and each enumerator replays the whole
// sequence from the start.
BEZEL_TEST(async_get_async_enumerator_owned_param_alias_2356,
           two_independent_enumerators_non_owned_param_reiterate) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    yield return 1; yield return 2; yield return 3; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var src := C.Gen(); "
        "    var sum1 := 0; "
        "    do "
        "      await using var en1 := src.GetAsyncEnumerator(); "
        "      var more := await en1.MoveNextAsync(); "
        "      while more do "
        "        sum1 := sum1 + en1.Current; "
        "        more := await en1.MoveNextAsync(); "
        "      end; "
        "    end; "
        "    var sum2 := 0; "
        "    do "
        "      await using var en2 := src.GetAsyncEnumerator(); "
        "      var more := await en2.MoveNextAsync(); "
        "      while more do "
        "        sum2 := sum2 + en2.Current; "
        "        more := await en2.MoveNextAsync(); "
        "      end; "
        "    end; "
        "    if sum1 <> 6 then return 1; end; "  // fresh replay: 1+2+3
        "    if sum2 <> 6 then return 2; end; "  // independent second replay
        "    return 0; "
        "  end Main; "
        "end C;"), 0);
}

// Adversarial self-pass (d), non-owned-param: two independent enumerators
// where the FIRST is abandoned mid-stream (break after one value) and the
// SECOND runs to full completion. Exercises the fix's guard twice in one
// method with different runtime identity outcomes (both false — neither
// `en1` nor `en2` aliases `src`, since neither call site involves an owned
// parameter) and confirms early abandonment of one enumerator does not
// disturb the source or a subsequent independent enumerator.
BEZEL_TEST(async_get_async_enumerator_owned_param_alias_2356,
           two_enumerators_first_abandoned_second_completes) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    yield return 1; yield return 2; yield return 3; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var src := C.Gen(); "
        "    var first := 0; "
        "    do "
        "      await using var en1 := src.GetAsyncEnumerator(); "
        "      var m1 := await en1.MoveNextAsync(); "
        "      if m1 then first := en1.Current; end; "
        "    end; "                            // en1 abandoned after 1 value
        "    var sum := 0; "
        "    do "
        "      await using var en2 := src.GetAsyncEnumerator(); "
        "      var more := await en2.MoveNextAsync(); "
        "      while more do "
        "        sum := sum + en2.Current; "
        "        more := await en2.MoveNextAsync(); "
        "      end; "
        "    end; "                            // en2 runs to completion
        "    if first <> 1 then return 1; end; "
        "    if sum <> 6 then return 2; end; "  // fresh replay: 1+2+3
        "    return 0; "
        "  end Main; "
        "end C;"), 0);
}

// The CLAIM mechanism (layer 1 of the fix, Async.cpp): a SECOND
// `.GetAsyncEnumerator()` call on the same named owned-param source raises
// `InvalidOperationException` — the first call already claimed the affine
// `c` capture (transferred into `en1`'s clone, `src`'s own copy nulled), so
// the second call sees the claim and raises rather than aliasing `en1` (the
// pre-fix behavior, and the double-free #2356 reported) or silently handing
// out a broken clone. `en1` itself is unaffected: it already advanced and
// read the first value (1) before the second call ever happens. Mirrors the
// sync iterator's own #973a/#2154 claim-and-IOE test
// (iterator_owned_param_conditional_973a_test.cpp,
// sync_second_enumeration_raises_and_disposes_once_spec_29_11) — same shape,
// async form. Cell.n == 0 after Main returns confirms the owned param is
// disposed exactly once (via en1's clone) despite the second call's failure
// — not leaked (the failed claim attempt allocates no clone to abandon) and
// not double-freed.
BEZEL_TEST(async_get_async_enumerator_owned_param_alias_2356,
           owned_param_second_get_call_raises_ioe_disposed_once) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Cell public static n: int := 0; public V: int := 1; "
        "  public constructor() Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "class P "
        "  public static async method UseCell(owned c: Cell): IAsyncEnumerable<int> "
        "    yield return c.V; "
        "    yield return c.V + 1; "
        "  end UseCell; "
        "  public static async method Main(): Task<int> "
        "    var a := 0; var caught := 0; "
        "    do "
        "      var src := P.UseCell(new Cell()); "
        "      await using var en1 := src.GetAsyncEnumerator(); "
        "      if await en1.MoveNextAsync() then a := en1.Current; end; "
        "      try "
        "        await using var en2 := src.GetAsyncEnumerator(); "
        "        var m2 := await en2.MoveNextAsync(); "
        "      catch InvalidOperationException => "
        "        caught := 1; "
        "      end; "
        "    end; "
        "    if Cell.n <> 0 then return 100; end; "  // exactly-once, no leak
        "    if a <> 1 then return 1; end; "
        "    if caught <> 1 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end P;"), 0);
}

// Non-owned-param control, isolating just the ordinary (no aliasing at all)
// case with a named source: confirms the fix's new AST-matching code
// (registerUsingDisposal, LlvmBackend_Setup.cpp) does not fire spuriously
// or misbehave when there is genuinely no owned parameter in play.
BEZEL_TEST(async_get_async_enumerator_owned_param_alias_2356,
           non_owned_param_named_source_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    yield return 42; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var src := C.Gen(); "
        "    await using var en := src.GetAsyncEnumerator(); "
        "    var m1 := await en.MoveNextAsync(); "
        "    if not m1 then return 1; end; "
        "    if en.Current <> 42 then return 2; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 0);
}

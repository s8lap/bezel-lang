// Ported from tests/sema/out_owned_condition_892_test.cpp
//   (BEZEL_TEST out_owned_condition_892, while_drain_dispose_body_accepted).
//
// §28.3 / #892 (WHILE DRAIN ACCEPT): the idiomatic drain loop
// `while s.TryPop(out owned var item) do item?.Dispose(); end`
// disposes item in the body each iteration and carries no obligation past the
// loop (false exit → TryPop returned false, item not written). Must ACCEPT.
//
// CLEAN ACCEPT (#2436 correction): `item` is a FRESH inline `out owned var`
// recipient minted by `TryPop` on every iteration, so `item?.Dispose()`
// discharges a genuinely-fresh Owned value each pass — there is no double
// dispose. This file previously pinned a spurious "disposed again" WARNING
// on the second/third loop pass as EXPECTED output; that warning was itself
// a symptom of #2436's bug (the pre-#2436-fix dataflow carried iteration
// N's terminal Disposed state, via the loop back-edge's union-key meet,
// into iteration N+1's entry instead of reseeding Owned for the fresh
// TryPop-minted value — see markOutOwnedArgsCore, Ownership_Checks.cpp).
// The original (pre-lit, C++) predecessor test
// (tests/sema/out_owned_condition_892_test.cpp, commit 633fcd4) only ever
// asserted `!hasErrors()`; the specific "disposed again" CHECK line was
// added later, mechanically, by the lit-conversion commit (a880656), which
// simply pinned whatever the (buggy) compiler emitted at the time — not a
// deliberate expectation. With #2436's fix, `item`'s state correctly resets
// to fresh Owned at the top of every iteration (mirroring how an ordinary
// `var w := new Widget();` inside a loop body already resets on every
// visit — see handleLocalDecl / seedOwnedIterVar), so the redundant-dispose
// warning no longer fires: assert it is now ABSENT.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class Cell public static _n: int := 0; public constructor() Cell._n := Cell._n + 1; end constructor; destructor Cell._n := Cell._n - 1; end destructor; end Cell; class C public static method Main(): int do using var s := new OwningStack<Cell>(); s.Push((new Cell())); s.Push((new Cell())); while s.TryPop(out var item) do item?.Dispose(); end; end; return Cell._n; end Main; end C;

// CHECK-NOT: disposed again here

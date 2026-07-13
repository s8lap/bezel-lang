// Ported from tests/sema/out_owned_condition_892_test.cpp
//   (BEZEL_TEST out_owned_condition_892, while_drain_dispose_body_accepted).
//
// §28.3 / #892 (WHILE DRAIN ACCEPT): the idiomatic drain loop
// `while s.TryPop(out owned var item) do item?.Dispose(); end`
// disposes item in the body each iteration and carries no obligation past the
// loop (false exit → TryPop returned false, item not written). Must ACCEPT.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class Cell public static _n: int := 0; public constructor() Cell._n := Cell._n + 1; end constructor; destructor Cell._n := Cell._n - 1; end destructor; end Cell; class C public static method Main(): int do using var s := new OwningStack<Cell>(); s.Push(move (new Cell())); s.Push(move (new Cell())); while s.TryPop(out owned var item) do item?.Dispose(); end; end; return Cell._n; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: local `item` is disposed again here — the second `.Dispose()` is a no-op (disposal is idempotent, §28.2); the redundant call usually indicates a bug

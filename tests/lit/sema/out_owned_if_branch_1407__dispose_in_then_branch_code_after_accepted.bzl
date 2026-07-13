// Ported from tests/sema/out_owned_if_branch_1407_test.cpp
//   (BEZEL_TEST out_owned_if_branch_1407, dispose_in_then_branch_code_after_accepted).
//
// §28.3 / #1407 (FALL-THROUGH + CODE AFTER): same as above but with additional
// code after the if-statement (another Push to verify the stack is usable).
// The join state after the if must be Disposed (not Owned), so there is no
// lingering obligation past the if — must ACCEPT.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Cell public static _n: int := 0; public constructor() Cell._n := Cell._n + 1; end constructor; destructor Cell._n := Cell._n - 1; end destructor; end Cell; class C public static method Main(): int do using var s := new OwningStack<Cell>(); s.Push(move (new Cell())); if s.TryPop(out owned var item) then item?.Dispose(); end; s.Push(move (new Cell())); end; return Cell._n; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §

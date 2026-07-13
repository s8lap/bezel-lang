// Ported from tests/sema/owned_iter_var_discharge_834_test.cpp
//   (BEZEL_TEST owned_iter_var_discharge_834, discharged_async_accepted).
//
// §29.2 / #834 (async over-reject guard): the async iteration variable
// discharged via move-out — must be ACCEPTED.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Box public constructor() end constructor; destructor end destructor; end Box; class C public static async method Gen(): IOwningAsyncEnumerable<Box> yield return new Box(); end Gen; public static async method Run(c: bool): Task await for var x in Gen() do using var y := move x; end; end Run; public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §

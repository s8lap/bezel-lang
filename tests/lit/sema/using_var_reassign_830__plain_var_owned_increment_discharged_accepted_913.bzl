// Ported from tests/sema/using_var_reassign_830_test.cpp
//   (BEZEL_TEST using_var_reassign_830, plain_var_owned_increment_discharged_accepted_913).
//
// ⚠ FLIPPED by fix #3978 (§25.1/§17.6, v0.295). Pre-fix (kept for history):
// "fix #913: the same owned `c++` is ACCEPTED when the local IS discharged
// (`.Dispose()`) ... Clean-compile ACCEPT control: bezelc -c must exit zero
// and emit no diagnostics." §25.1/§17.6 retire `Counter`'s `operator ++`
// declaration and the `c++` use independently of whether `c` is later
// discharged — discharge is irrelevant once the statement itself cannot
// compile.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class Counter public Value: int := 0; public constructor(v: int) Value := v; end constructor; destructor Value := 0; end destructor; public static method operator ++(c: Counter): owned Counter => new Counter(c.Value + 1); end Counter; class C public static method Main(): int var c := new Counter(0); c++; c.Dispose(); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `++` may not be overloaded (§25.1)
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `++` is not defined on reference type `Counter`
// CHECK-SAME: not user-overloadable (§25.1)

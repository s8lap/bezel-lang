// Ported from tests/sema/using_var_reassign_830_test.cpp
//   (BEZEL_TEST using_var_reassign_830, plain_var_owned_increment_accepted_1465).
//
// ⚠ FLIPPED by fix #3978 (§25.1/§17.6, v0.295). Pre-fix (kept for history):
// "§17.6 / §28.7 drop-on-overwrite / §28.3 auto-drop (#913 updated by #1465):
// `var c := new T(); c++` is modelled as the reassignment
// `c := operator++(c)` ... ACCEPTED ... with only the must-use warning."
// That reading is the v0.236 rule §25.1/§17.6 retire at v0.295: `++`/`--`
// are never user-overloadable, so `Counter`'s own `operator ++` declaration
// is now rejected (§25.1) and `c++` is independently rejected at its use
// site too (§17.6) — the program that used to compile clean (module the
// must-use warning) is now a REJECT.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class Counter public Value: int := 0; public constructor(v: int) Value := v; end constructor; destructor Value := 0; end destructor; public static method operator ++(c: Counter): owned Counter => new Counter(c.Value + 1); end Counter; class C public static method Main(): int var c := new Counter(0); c++; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `++` may not be overloaded (§25.1)
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `++` is not defined on reference type `Counter`
// CHECK-SAME: not user-overloadable (§25.1)

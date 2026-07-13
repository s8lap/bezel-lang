// Ported from tests/sema/using_var_reassign_830_test.cpp
//   (BEZEL_TEST using_var_reassign_830, plain_var_owned_increment_accepted_1465).
//
// §17.6 / §28.7 drop-on-overwrite / §28.3 auto-drop (#913 updated by #1465):
// `var c := new T(); c++` is modelled as the reassignment `c := operator++(c)`,
// which drops the previous owned value. With §28.7 drop-on-overwrite now covering
// plain owned class locals (including the increment target — codegen disposes the
// displaced value at the `c++` store, LlvmBackend_Stmts.cpp), the reassignment is
// ACCEPTED: the old Counter is disposed at the overwrite and the final Counter
// auto-drops at scope end (each exactly once), with only the must-use warning.
// This is the #1465 correction of #913's original blanket rejection — verified
// runtime-sound (old value disposed, then new value, no double-free). A BORROWED
// increment-reassignment is instead caught by the consume-site freeze (§28.3).
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class Counter public Value: int := 0; public constructor(v: int) Value := v; end constructor; destructor Value := 0; end destructor; public static method operator ++(c: Counter): owned Counter => new Counter(c.Value + 1); end Counter; class C public static method Main(): int var c := new Counter(0); c++; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: owned value in `c` is implicitly disposed at the end of its scope
// CHECK-SAME: (§28.3 auto-drop)

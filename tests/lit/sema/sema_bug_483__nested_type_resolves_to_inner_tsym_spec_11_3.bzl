// Ported from tests/sema/bug_483_nested_type_construction_test.cpp
//   (BEZEL_TEST sema_bug_483, nested_type_resolves_to_inner_tsym_spec_11_3).
//
// The inner TypeSymbol is the one sema should attach: not the outer one.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class Outer public class Inner public _v: int := 0; destructor end destructor; end Inner; end Outer; class C public static method Main(): int var i := new Outer.Inner(); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} owned value in `i` is implicitly disposed at the end of its scope
// CHECK-SAME: (§28.3 must-use / auto-drop)

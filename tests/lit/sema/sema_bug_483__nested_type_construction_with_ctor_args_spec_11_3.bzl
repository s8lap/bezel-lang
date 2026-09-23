// Ported from tests/sema/bug_483_nested_type_construction_test.cpp
//   (BEZEL_TEST sema_bug_483, nested_type_construction_with_ctor_args_spec_11_3).
//
// §11.3: `new Outer.Inner(args)` with an explicit constructor in Inner.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class Outer public class Inner public _v: int := 0; public constructor(v: int) _v := v; end constructor; destructor end destructor; end Inner; end Outer; class C public static method Main(): int var i := new Outer.Inner(42); return i._v; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} owned value in `i` is implicitly disposed at the end of its scope
// CHECK-SAME: (§28.3 auto-drop)

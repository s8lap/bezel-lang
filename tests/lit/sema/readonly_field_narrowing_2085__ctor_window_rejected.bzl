// §7.4 (v0.214, proposal 0058, #2085): the rule does not apply inside the
// declaring type's construction window (the §22.8 assignment window). Inside the
// constructor body a `_name <> null` guard narrows nothing, so the field access
// is REJECTED even though `_name` is readonly and already assigned here.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public readonly _name: string?; public constructor(n: string?) _name := n; if _name <> null then var x := _name.Length; end; end constructor; destructor end destructor; public static method Main(): int using var c := new C("hi"); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Length` on nullable field `_name` requires a local copy first
// CHECK-SAME: (§7.4)

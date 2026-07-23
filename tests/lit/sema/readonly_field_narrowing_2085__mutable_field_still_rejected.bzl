// §7.4 (v0.214, proposal 0058, #2085): `readonly` is the discriminator. A
// MUTABLE nullable field is unchanged — the blanket TOCTOU refusal still stands
// (a writer through another alias could null it between check and use). It must
// still be REJECTED even though the same shape with a `readonly` field is now
// accepted.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public _name: string?; public constructor(n: string?) _name := n; end constructor; destructor end destructor; public method Len(): int if _name <> null then return _name.Length; end; return 0; end Len; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Length` on nullable field `_name` requires a local copy first
// CHECK-SAME: (§7.4)

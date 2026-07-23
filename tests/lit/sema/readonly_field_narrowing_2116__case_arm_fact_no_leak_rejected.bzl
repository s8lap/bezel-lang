// §7.4 (v0.214) / #2116: case arms are ALTERNATIVES — a readonly-field fact
// established inside one arm's `do … end` body (here arm 1's early-return
// guard) must not be visible to a later arm's analysis: arm 2 executes exactly
// when arm 1 did NOT, so its unguarded `_name.Length` is REJECTED. Each arm's
// field-fact state starts from the case-entry state. (The legacy local sets
// leak arm-to-arm here — a pre-existing hole filed separately.)
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public readonly _name: string?; public constructor(n: string?) _name := n; end constructor; destructor end destructor; public method Len(k: int): int case k of 1 => do if _name = null then return 0; end; return 10; end, 2 => do return _name.Length; end, _ => do return 5; end end; return 6; end Len; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Length` on nullable field `_name` requires a local copy first
// CHECK-SAME: (§7.4)

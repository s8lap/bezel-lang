// §7.4 (v0.214) / #2116: a catch arm runs when the try body was interrupted at
// ANY point — possibly before the fact-establishing guard executed — so a
// readonly-field fact established INSIDE the try body is not visible in the
// catch arm. The catch body's unguarded `_name.Length` is REJECTED. (The
// legacy local sets leak try-body facts into catch arms — a pre-existing hole
// filed separately.)
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public readonly _name: string?; public constructor(n: string?) _name := n; end constructor; destructor end destructor; public method Noise() end Noise; public method Len(): int try if _name = null then return 0; end; Noise(); catch Exception => return _name.Length; end; return 1; end Len; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Length` on nullable field `_name` requires a local copy first
// CHECK-SAME: (§7.4)

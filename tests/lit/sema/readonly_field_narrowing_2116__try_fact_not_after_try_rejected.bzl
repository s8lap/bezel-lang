// §7.4 (v0.214) / #2116: the statement after a try/catch is reached either
// from the try body's completion OR from a catch arm's fall-through — and on
// the catch path the try body's fact-establishing guard may never have run.
// The after-try state is after-body ∩ each catch exit, so the fact established
// inside the try body does not survive and the after-try `_name.Length` is
// REJECTED.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public readonly _name: string?; public constructor(n: string?) _name := n; end constructor; destructor end destructor; public method Noise() end Noise; public method Len(): int try if _name = null then return 0; end; Noise(); catch Exception => var y := 0; end; return _name.Length; end Len; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot narrow `T?` accessed through a field
// CHECK-SAME: (§7.4)

// §7.4 (v0.214) / §31.2 / #2116 (pre-gate review): the FINALLY body runs on
// every exit from the construct — including the try body's own EARLY RETURN
// on the null branch of its guard (`if _name = null then return 0; end;`
// runs the finally with `_name` NULL), and an unmatched-exception unwind.
// A fact established inside the try body (and re-established in the catch
// arm) is therefore NOT valid at finally entry; the finally's unguarded
// access is REJECTED. (Probe-confirmed SEGV on `new C(null)` via the plain
// early-return path when the fact was admitted.)
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public readonly _name: string?; public constructor(n: string?) _name := n; end constructor; destructor end destructor; public method Noise() end Noise; public method Len(): int try if _name = null then return 0; end; Noise(); catch ArgumentException => do if _name = null then return 1; end; end; finally var x := _name.Length; end; return 2; end Len; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot narrow `T?` accessed through a field
// CHECK-SAME: (§7.4)

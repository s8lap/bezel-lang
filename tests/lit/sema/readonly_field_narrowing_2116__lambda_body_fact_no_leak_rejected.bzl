// §7.4 (v0.214) / #2113 / #2116: a readonly-field fact established INSIDE a
// lambda body must NOT leak to the enclosing scope — the lambda body is
// deferred code that may run never or later, so the enclosing region has no
// evidence the test executed. The `_name = null` early-return guard inside the
// lambda narrows only the lambda's own remainder; the enclosing `_name.Length`
// is unguarded and REJECTED. (This is the #2113 hole, closed by construction
// for the field-fact set: the lambda visit restores the enclosing set.)
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public readonly _name: string?; public constructor(n: string?) _name := n; end constructor; destructor end destructor; public method Noise() end Noise; public method Use(): int var f := lambda() => do if _name = null then return; end; Noise(); end; return _name.Length; end Use; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot narrow `T?` accessed through a field
// CHECK-SAME: (§7.4)

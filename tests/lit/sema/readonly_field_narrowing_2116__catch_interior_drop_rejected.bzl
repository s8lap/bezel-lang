// §7.4 (v0.214) / §31.2 / #2116 (pre-gate review): the try body can be
// interrupted at ANY point, and the entry ∩ after-body intersection is blind
// to an INTERIOR drop that a later re-establishment masks: `a := Get()`
// drops the entry fact, the throw window opens while `a._f` may be null,
// and the body's fall-through re-guard re-establishes the same key. The
// catch arm therefore must not carry the entry fact once a drop edge fired
// inside the body; its unguarded `a._f.Length` is REJECTED.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class H public readonly _f: string?; public constructor(f: string?) _f := f; end constructor; destructor end destructor; end H; class C public static method Get(): owned H => new H(null); public static method M(cond: bool): int var a := new H("xy"); if a._f <> null then try a := Get(); if cond then throw new ArgumentException("boom"); end; if a._f = null then return 0; end; catch ArgumentException => do return a._f.Length; end; end; end; return 1; end M; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Length` on a nullable field or property requires narrowing to non-null first
// CHECK-SAME: (§7.4)

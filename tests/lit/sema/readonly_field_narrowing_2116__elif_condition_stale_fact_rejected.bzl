// §7.4 (v0.214) / #2116: an elif CONDITION executes only when every preceding
// condition was FALSE — so a fact from the preceding TRUE-branch body must not
// be visible while the elif condition is checked. Here the `then` body ran
// under `_name <> null`; the elif condition runs exactly when that guard was
// false (`_name` is null), so its unguarded `_name.Length` is REJECTED.
// (The field-fact walk seeds each elif condition with entry + the preceding
// conditions' false-branch facts before visiting it; the legacy local sets
// skip that reset — a pre-existing local-narrowing hole filed separately.)
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public readonly _name: string?; public constructor(n: string?) _name := n; end constructor; destructor end destructor; public method Noise() end Noise; public method Len(): int if _name <> null then Noise(); return 100; elif _name.Length > 0 then return 1; end; return 0; end Len; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot narrow `T?` accessed through a field
// CHECK-SAME: (§7.4)

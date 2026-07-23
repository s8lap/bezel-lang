// §7.4 (v0.214) / #2114 / #2116: a `static readonly` field of a GENERIC type
// does not narrow through the `TypeName._s` root — conservatively declined.
// Sema shares one FieldSymbol across instantiations while each instantiation
// has separate static storage (per-instantiation statics confirmed by probe on
// #2114), so a shared narrowing key would let a fact about `Cache<int>._label`
// wrongly admit `Cache<string>._label`. Until keys are instantiation-aware,
// the guard establishes nothing even for the SAME spelled instantiation:
// REJECTED — copy to a local (the standard §7.4 remedy).
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class Cache<T> public static readonly _label: string? := "hi"; end Cache; class C public static method Main(): int if Cache<int>._label <> null then return Cache<int>._label.Length; end; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Length` on a nullable field or property requires narrowing to non-null first
// CHECK-SAME: (§7.4)

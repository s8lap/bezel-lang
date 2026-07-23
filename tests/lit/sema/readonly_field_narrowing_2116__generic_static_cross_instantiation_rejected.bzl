// §7.4 (v0.214) / #2114 / #2116: the cross-instantiation attack shape the
// #2114 probe warned about. `Cache<int>._label` and `Cache<string>._label`
// are SEPARATE static storage (per-instantiation statics — probe-confirmed),
// but sema shares one FieldSymbol for `_label` across instantiations; a
// narrowing key blind to the instantiation would let the `Cache<int>` guard
// admit the `Cache<string>` access — reading a possibly-null unrelated slot.
// Generic-type static roots are declined wholesale, so this is REJECTED.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class Cache<T> public static readonly _label: string? := "hi"; end Cache; class C public static method Main(): int if Cache<int>._label <> null then return Cache<string>._label.Length; end; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: member access `.Length` on a nullable field or property requires narrowing to non-null first
// CHECK-SAME: (§7.4)

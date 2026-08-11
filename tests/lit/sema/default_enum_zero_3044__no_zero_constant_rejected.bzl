// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #3044 (§17.11, v0.242): "`default(T)` is a compile error for … an enum
// type with no declared constant of underlying value 0." Escalated from a
// warning by proposal 0078's Q3 rider — `default(E)` was the LAST producer of
// an enum value equal to no declared constant, and §5.4's invariant ("there is
// no value of an enum type that is not equal to a declared constant") — which
// §19.6's exhaustiveness guarantee and §21.7's totality grant both rest on — is
// only true with it closed. "The remedy is declaring a zero-valued constant."
//
// `Level` declares 1, 2, 3, so no constant has underlying value 0.

module M; enum Level Low := 1, Mid := 2, High := 3; end Level; class C public static method Main(): int var l: Level := default(Level); return int(l); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `default(T)` is a compile error for enum `Level` — it declares no constant of underlying value 0
// CHECK-SAME: (§17.11)

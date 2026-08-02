// §22.12 rule 2 (v0.221 ruling / #1393, spec v0.228): a mutable static
// FIELD of function type is a compile error — the ruling's migration note
// names "function-typed... statics" among the newly-rejected residual
// categories. Previously silently admitted with no diagnostic.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; class App public static method Twice(x: int): int return x * 2; end Twice; public static _f: (int) -> int := Twice; public static method Main(): int => 0; end App;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: mutable static field `_f` has type `<function type>`, which §22.12 rule 2 does not admit as a mutable static — function-typed statics are not in the admitted category set
// CHECK-SAME: (§22.12 rule 2, v0.221, #1393)

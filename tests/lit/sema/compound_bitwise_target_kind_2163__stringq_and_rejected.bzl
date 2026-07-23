// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2163 (§6.5 / §3.5, spec v0.220): a `string?` TARGET of a compound bitwise
// op with an integer RHS. The #2153 `string?`-target reject covers the
// arithmetic chain; the bitwise/shift branch relies on the §6.5 target-kind
// check, which was missing string: before the fix `string?(null) &= 2`
// reached codegen and aborted on malformed IR (`and ptr, i32` — probe,
// main @dd33abd). Rejects identically to the non-nullable form (the check
// reads the underlying primitive kind, as for `char?`).

module M; class C public static method Main(): int var s: string? := null; s &= 2; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `&=` is not valid on `string` — bitwise and shift operators require integer operands
// CHECK-SAME: (§6.5)

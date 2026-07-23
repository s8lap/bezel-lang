// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2163 (§6.5 / §3.5, spec v0.220): a `decimal` TARGET of a compound bitwise
// op. §3.5 lists bitwise on the eight integer primitives only — decimal is
// not among them (it is not an integer type; §6.5's operators act on "the
// binary representation of integer values"). The §6.5 target-kind check
// missed Decimal (the RHS-side list already rejected a decimal RHS): before
// the fix `decimal(1.5m) &= 3` compiled clean and was a silent NO-OP at
// runtime (emitDecimalArith defines no `&`, so the store was skipped —
// probe, main @dd33abd). The badKind spelling matches the RHS-side list,
// which groups decimal with the floating-point rejects.

module M; class C public static method Main(): int var d: decimal := 1.5m; d &= 3; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: operator `&=` is not valid on floating-point types — bitwise and shift operators require integer operands
// CHECK-SAME: (§6.5)

// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2155 (§6.2/§6.6, spec v0.226): `int += ulong` — the §6.2-inadmissible
// mixed-signedness pair. §5.1: "never implicit: between signed and unsigned
// types of the same or narrower width"; `int` and `ulong` have no implicit
// conversion in either direction, so the binary `int + ulong` is itself a
// §6.2 compile error, and the compound `int += ulong` (desugared to
// `int := int + ulong`, §6.6) is the same error. Before the fix the §6.6
// family check's "no conversion either way" branch set rPrim = rTy->primitive
// and the integerWriteBack clause then admitted ANY integer/integer pair —
// including this §6.2-inadmissible one — so it compiled and raised
// OverflowException at runtime (SExt(-1) fed to an unsigned i64 add).

module M; class C public static method Main(): int var i: int := -1; var u: ulong := 2UL; i += u; return i; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: compound assignment `+=` has no implicit conversion between operand types `int` and `ulong`
// CHECK-SAME: (§6.2/§6.6)

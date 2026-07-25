// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2155 (§6.2/§6.6, spec v0.226): `ulong &= int` — the bitwise (non-shift)
// compound carries the SAME §6.2 pair-admissibility as the binary `ulong & int`
// (typeBinary runs the §6.2 promotion for `&`/`|`/`^`). §5.1 admits no implicit
// conversion between `ulong` and `int` in either direction, so both forms are
// §6.2 compile errors. Before the fix the bitwise branch of the §6.6 family
// check tested operand KINDS only (§6.5: both integer — passes), with no
// pair-admissibility, so `ulong &= int` compiled and stored a value computed
// under a conversion the spec does not define. (Shift forms are exempt: the
// count does not participate in §6.2 promotion, §6.5.)

module M; class C public static method Main(): int var v: ulong := 10UL; var w: int := -2; v &= w; return int(v); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: compound assignment `&=` has no implicit conversion between operand types `ulong` and `int`
// CHECK-SAME: (§6.2/§6.6)

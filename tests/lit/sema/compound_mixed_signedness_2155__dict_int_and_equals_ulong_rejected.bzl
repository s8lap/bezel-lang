// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2155 (§6.2/§6.6, spec v0.226): dict-indexer compound `d["a"] &= w` on an
// `int` dict value with a `ulong` VARIABLE RHS — the bitwise (non-shift) twin
// of the `|=` cell. Same §6.2 pair-admissibility as the binary `int & ulong`
// (typeBinary runs §6.2 promotion for `&`/`|`/`^`): the pair (`int`, `ulong`)
// has no implicit conversion in either direction (§5.1), so both the binary
// and the compound (desugared `x := x op e`, §6.6) are §6.2 compile errors.
//
// #2161 regression coverage moved from the codegen track: the former
// `codegen_2161_var_ulong.and_2_31_raises` cell asserted a runtime
// OverflowException (the OLD WRONG behavior — the compound was admitted and
// raised at runtime). #2155 makes the compound match the binary's §6.2 reject,
// so the runtime cell compile-rejects and moved here. The RHS is a variable,
// so the §5.1 constant-narrowing / literal-derived exemption does not apply.

module M; class C public static method Main(): int var d := new Dictionary<string, int>(); d["a"] := -1; var w: ulong := 2147483648UL; d["a"] &= w; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: compound assignment `&=` has no implicit conversion between operand types `int` and `ulong`
// CHECK-SAME: (§6.2/§6.6)

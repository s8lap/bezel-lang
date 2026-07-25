// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2155 (§6.2/§6.6, spec v0.226): dict-indexer compound `d["a"] |= w` on an
// `int` dict value with a `ulong` VARIABLE RHS. The pair (`int`, `ulong`) has
// no implicit conversion in either direction (§5.1 — `int` widens only to
// long/float/double/decimal; `ulong` widens only to float/double/decimal; the
// signed↔unsigned pair is "never implicit" both ways), so the binary
// `int | ulong` is itself a §6.2 compile error, and the compound form
// (desugared `x := x op e`, §6.6) is the same compile error. The §6.6
// narrow-integer write-back (§5.2 checked narrowing) never reaches codegen:
// the desugared binary has no result type to narrow.
//
// This is the #2161 regression coverage that the --full gate caught: the two
// `codegen_2161_var_ulong` cells (dict_int `|= ulong-var` / `&= ulong-var`)
// previously asserted a runtime OverflowException — the OLD WRONG behavior
// (#2155's §6.6 family check admitted the compound, diverging from the binary
// `int | ulong` which already rejected, then codegen raised at runtime). The
// §6.2 pair-admissibility fix makes the compound match the binary, so the
// runtime cells compile-reject and were moved here to the lit sema track. The
// RHS is a variable (not literal-derived), so the §5.1 constant-narrowing
// exemption does not apply.

module M; class C public static method Main(): int var d := new Dictionary<string, int>(); d["a"] := 0; var w: ulong := 4294967296UL; d["a"] |= w; return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: compound assignment `|=` has no implicit conversion between operand types `int` and `ulong`
// CHECK-SAME: (§6.2/§6.6)

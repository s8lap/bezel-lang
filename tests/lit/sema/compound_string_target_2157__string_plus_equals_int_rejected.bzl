// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2157 (§4.6/§6.6, spec v0.226): `s += 5` — a non-nullable `string` target
// with a non-string RHS. §4.6: "`s + t` concatenates two strings. The compiler
// does not implicitly convert other types to `string`." The binary `s + 5` is
// rejected (§4.6), so the compound `s += 5` (desugared to `s := s + 5`, §6.6)
// is the same error. Before the fix the §6.6 family check's arithmetic branch
// skipped a String target entirely (`else if (tTy->primitive != P::String)`),
// so this was silently admitted and the #857 codegen path's retired
// `string + T` rule rendered the int (printed "v=5").

module M; class C public static method Main(): int var s: string := "v="; s += 5; return s.Length; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: compound assignment `+=` on a `string` target requires a `string` right-hand operand
// CHECK-SAME: (§4.6/§6.6)

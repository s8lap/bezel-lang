// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2157 (§4.6/§6.6, spec v0.226): `s -= t` — no `-` operator is defined on
// strings (§4.6 defines only `+` for concatenation; §3.5 lists the numeric
// types only). The compound `s -= t` (desugared to `s := s - t`, §6.6) has no
// result and must be a clean compile error. Before the fix the §6.6 family
// check admitted this (the String-target case fell through every branch) and
// codegen aborted: `llvm.ssub.with.overflow` on string POINTERS failed LLVM
// IR verification (a compiler CRASH).

module M; class C public static method Main(): int var s: string := "abc"; var t: string := "b"; s -= t; return s.Length; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: compound assignment `-=` is not defined on a `string` target
// CHECK-SAME: (§4.6/§6.6)

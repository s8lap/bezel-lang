// RUN: %bezelc -c -o %t.o %s
//
// §35.5 / §9.7 (v0.220, issue #2180): the ACCEPT (non-over-rejection) control
// for the route-2 arm-binding provenance — a case-arm payload binding whose
// scrutinee's carried source is a PARAMETER (`a`) is NOT escape-bound, so the
// widened arm-binding provenance (x -> b -> hh -> a) resolves no scope-bound
// source and the returned closure capturing `x` must stay ACCEPTED (a parameter
// outlives the callee frame, exactly as the #2144 param-sourced record accept).
// The over-reject failure mode is a spurious compile error here; this pins that
// it does not occur. (A compile-only accept: a RETURNED closure capturing a
// case-arm binding trips a pre-existing, orthogonal codegen leak of the capture
// environment — present on main, unrelated to this ownership fix — so the
// runtime path is not exercised here.) // per §35.5, §9.7 (v0.220)

module M; record Holder(S: ArraySpan<int>); choice Box Got(h: Holder), Nothing; end Box; class C public static method Make(a: Array<int>): () -> int var hh := new Holder(a.AsSpan()); var b := Box.Got(hh); case b of Got(var x) => do return lambda(): int => x.S[0]; end, Nothing => do end end; return lambda(): int => 0; end Make; public static method Main(): int var arr := new Array<int>(); arr.Add(7); var f := C.Make(arr); return f(); end Main; end C;

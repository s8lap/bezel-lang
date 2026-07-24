// RUN: %bezelc -c -o %t.o %s
//
// §35.4.3 / §35.5 (v0.220, issue #2180): the ACCEPT (non-over-rejection)
// control for a VALUE-LIKE choice's arm binding. `x` is an int COPY, not a
// borrow-carrying binding, so the arm-binding provenance registration must NOT
// record it (it gates on the borrow-bearing-carrier predicate) and the returned
// closure capturing it must stay ACCEPTED — the carrier predicate must not
// widen to "any choice / any arm binding". (Compile-only: a returned closure
// capturing a case-arm binding trips a pre-existing, orthogonal codegen leak of
// the capture environment, unrelated to this ownership fix.) // per §35.4, §35.5

module M; choice VBox Some(n: int), None; end VBox; class C public static method Make(): () -> int var b := VBox.Some(42); case b of Some(var x) => do return lambda(): int => x; end, None => do end end; return lambda(): int => 0; end Make; public static method Main(): int var f := C.Make(); return f(); end Main; end C;

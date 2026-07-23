// ANTI-OVER-REJECT (§30.1 / §9.7, v0.220, issue #2145): a generic record
// instantiated at a RUNTIME-MANAGED type argument (`Wrap<string>`) — §9.7:
// "Reference-typed fields whose types are runtime-managed (`string`, function
// types, boxed value types) do not make the record borrow-bearing because
// runtime-managed values have unbounded lifetime by construction." The
// substituting field walk must preserve that carve-out at the substituted
// type, so the async instantiation stays legal.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Wrap<T>(V: T); static class F public static async method Work<T>(x: T): Task<T> return x; end Work; end F; class C public static method Main(): int var p := new Wrap<string>("ok"); using var t := F.Work<Wrap<string>>(p); return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §

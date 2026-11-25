// fix #9021 (§8.4 / §27.8, v0.359): the type-ALIAS spelling of the root's
// ONE static — `KA.ReferenceEquals(a, b)` — warns on a value-type argument
// too. §27.8: "Type aliases — generic or not — see through to the aliased
// type for identity, equality, method dispatch, and overload resolution", so
// the alias names the SAME call the root spelling names — and §8.4's "The
// compiler warns on any value-typed argument" is that call's warning. Without
// this the alias spelling would be a way to SILENCE it.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is
// legal), but it DOES emit a diagnostic — pinned below so the warning itself
// stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; enum Color Red, Green, Blue; end Color; class K end K; type KA := K; class C public method M(a: Color, b: Color): bool => KA.ReferenceEquals(a, b); end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} `Object.ReferenceEquals` is called with a value-type argument; a non-null value is boxed into a fresh object on every call, so the comparison is always false for it — a null nullable value boxes to a null reference instead and compares true only against another null
// CHECK-SAME: (§8.4/§8.6)

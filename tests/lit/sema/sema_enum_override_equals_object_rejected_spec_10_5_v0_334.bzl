// fix #5633 (§10.5, v0.334): `override Equals(x: Object)` (non-nullable) on
// an enum is not one of the admitted replacement members. §10.5 names only:
// `Equals(E)` (the enum's own type), `Equals(Object?)` (admitted per §9.2
// / #5598), `GetHashCode()`, and `ToString()`. A parameter type of `Object`
// (non-nullable) is not admitted; only `Object?` is.
//
// This row pins the narrower distinction that b530bf05 introduced vs the
// pre-existing arity-only predicate: the wrong-type rows all use `int` (or
// another non-enum/non-Object? type), which the earlier arity-1 check already
// rejected. This row uses `Object` (non-nullable) specifically, which the
// earlier check DID NOT reject (arity 1 matched), but the narrowed predicate
// now does.
//
// REJECT: `override Equals` with non-nullable Object parameter on enum.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; enum Color Red, Green, Blue; public override method Equals(x: Object): bool => x <> null; public override method GetHashCode(): int => 1; end Color; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: enum method `Equals` may not take `Object` — an enum's `Object` slot is filled by its own-typed `Equals(other: Color)`, spelled `override` or plain (§10.5)

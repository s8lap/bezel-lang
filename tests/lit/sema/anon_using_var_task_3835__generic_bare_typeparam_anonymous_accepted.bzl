// Issue #3835 / spec §17.13 (v0.288, axiom A6) / K2.4 pre-substitution
// keying: false-reject control. The rejection is keyed on the binding's
// TYPE AT THE SITE, pre-substitution — a bare (unconstrained) generic type
// parameter is not `Task`-shaped, so an anonymous `using var _` over a
// bare-`T`-typed value inside a generic method body must NOT be rejected,
// regardless of what concrete type a caller might later instantiate `T`
// with. (A bare type parameter IS a `TypeRef::Kind::Named` -- with a null
// declaration -- so it passes that test; what excludes it is
// `isTaskShapedRef`'s `headName()` check, since `T` is neither "Task" nor
// "OwningTask", and so it is never Task-shaped at
// this check, matching the pre-existing `_ := expr;` discard check this fix
// reuses the same predicate from.)
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no
// diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; static class F public static method Hold<T>(x: T): int using var _ := x; return 0; end Hold; end F; class C public static method Main(): int return F.Hold<int>(0); end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §

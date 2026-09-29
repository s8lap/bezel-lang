// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// spec §14.2, v0.307 / #4689: the constraint table admits exactly two
// NAMED-TYPE `where T: X` forms — `where T: SomeClass` ("Class types
// only") and `where T: IFoo` (interface conformance) — plus the type-
// parameter bound `where T: U`. A bound that resolves to a RECORD is not
// among these; grammar A.3's catch-all `Constraint ::= ... | Type`
// alternative admits it SYNTACTICALLY, but §14.2 never describes such a
// bound semantically (every cross-reference to a concrete-type bound
// elsewhere in the spec speaks only of "a class bound" or "an interface
// bound"). Rejected at the declaration, independent of instantiation —
// `Box<T>` is never instantiated in this program, so only a declaration-
// level check (not the per-instantiation one) can catch it.
//
// Fails-without-fix: before #4689 this compiled rc=0 with no diagnostic —
// the bound silently resolved and `Box<T>.Describe`'s `case t of
// Point(x, y) => ...` drew a spurious §19.6 non-exhaustiveness warning
// instead (a WARNING, so `%not` would still see rc=0; #4689's own repro).

module M;

public record Point(X: int, Y: int);

public class Box<T> where T: Point
  public method Describe(t: T): int => (case t of
    Point(x, y) => x + y);
end Box;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type parameter `T` is constrained to `Point`, a record
// CHECK-SAME: (§14.2)

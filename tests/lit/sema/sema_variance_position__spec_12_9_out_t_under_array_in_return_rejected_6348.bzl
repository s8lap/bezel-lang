// fix #6348 (§12.9, v0.338): the composed position check.
//
// The invariant-wrapper arm of the row above, on a RETURN slot: `Array<T>` is
// a class and so always invariant (§12.9), so `out T` is unhonourable there
// too — a caller holding `I<Dog>` could not hand back the `Array<Animal>` the
// variance promises.
//
// MEASURED before the fix: this compiled clean (silent acceptance).
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; interface I<out T> method Get(): Array<T>; end I;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type parameter `T` is covariant (`out`) but appears in an input (parameter) position (composed through a nested type constructor) in method `Get`
// CHECK-SAME: (§12.9)

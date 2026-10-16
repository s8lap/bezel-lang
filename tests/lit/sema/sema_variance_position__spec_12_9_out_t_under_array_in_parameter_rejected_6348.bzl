// fix #6348 (§12.9, v0.338): the composed position check.
//
// §12.9: "Position is composed across nested type constructors. A type
// parameter inside another generic type inherits position from that wrapper's
// variance … The compiler enforces position constraints." `Array<T>` is a
// class, so it is "always invariant" (§12.9) — `out T` cannot be honoured
// through it in ANY slot, least of all a method parameter (an input).
//
// MEASURED before the fix: this compiled clean (silent acceptance).
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; interface I<out T> method Put(xs: Array<T>); end I;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type parameter `T` is covariant (`out`) but appears in an input (parameter) position (composed through a nested type constructor) in parameter `xs` of method `Put`
// CHECK-SAME: (§12.9)

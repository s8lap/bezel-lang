// fix #6348 (§12.9, v0.338): the composed position check.
//
// §12.9: "Function types are themselves variant — covariant in return type,
// contravariant in parameter types." `() -> T` is therefore COVARIANT in T;
// sitting in a method parameter (an input slot) that composes to
// CONTRAVARIANT, which `out T` cannot honour. The mirror of the accepted
// `IEvent<out T>.Subscribe(h: (T) -> ())` row, where contravariant-of-
// contravariant lands on covariant.
//
// MEASURED before the fix: this compiled clean (silent acceptance).
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; interface I<out T> method Register(f: () -> T); end I;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type parameter `T` is covariant (`out`) but appears in an input (parameter) position (composed through a nested type constructor) in parameter `f` of method `Register`
// CHECK-SAME: (§12.9)

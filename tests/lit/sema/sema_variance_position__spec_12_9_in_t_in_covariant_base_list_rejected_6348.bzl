// fix #6348 (§12.9, v0.338): the composed position check, on a BASE list.
//
// §12.9: "The extending interface instantiates the base's variant parameter
// with its own type parameter, holding the deriving interface's variance."
// `IEnumerable` is `out`, so a contravariant `T` cannot be passed as its
// argument — `Bad<B>` → `Bad<A>` for `A <: B` would need `IEnumerable<B>` →
// `IEnumerable<A>`, which is the wrong direction.
//
// MEASURED before the fix: this compiled clean (silent acceptance).
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M; interface Bad<in T>: IEnumerable<T> end Bad;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type parameter `T` is contravariant (`in`) but appears in base type `IEnumerable`, whose own parameter variance cannot honour that position (composed through a nested type constructor)
// CHECK-SAME: (§12.9)

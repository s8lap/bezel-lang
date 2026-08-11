// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #3044 (§10.4 / stdlib §9.9, v0.242): "The parameter type is the
// underlying type `U` exactly: a wider integer in hand is narrowed explicitly
// first (§5.2, checked), keeping the two failure modes — out of `U`'s range vs.
// not a declared value — separately visible." `Priority`'s underlying type is
// the default `int` (§10.3), and a run-time `long` does not implicitly convert
// to `int` (§5.1's never-implicit list), so the call is rejected; the remedy is
// the explicit checked narrowing `int(w)`.

module M;

enum Priority Low, Medium, High; end Priority;

class C public static method Main(): int
  var w: long := 1;
  var p := Priority.FromUnderlyingOrNull(w);
  return (if p = null then 1 else 0);
end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: {{.*}}argument to `Priority.FromUnderlyingOrNull`

// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #3862 (§10.4, v0.293; stdlib §9.9, v0.293): §10.4 declares
// `public static method NameOf(value: E): string` — the parameter is the ENUM
// TYPE, not its underlying integer (that is `FromUnderlyingOrNull`'s
// parameter, §10.4's "The parameter type is the underlying type `U` exactly").
// An `int` argument is not an enum value, so the call is rejected (§5.1).

module M;

enum Color Red, Green; end Color;

class C public static method Main(): int
  var s := Color.NameOf(1);
  return 0;
end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: {{.*}}argument to `Color.NameOf`

// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #3862 (§10.4 "Methods, properties, static members", v0.293; stdlib §9.9,
// v0.293): `E.FromNameOrNull(name: string): E?` — the parameter is `string`,
// so an `int` argument is not implicitly convertible to it (§5.1's
// never-implicit list). The remedy is the string itself.

module M;

enum Color Red, Green; end Color;

class C public static method Main(): int
  var p := Color.FromNameOrNull(1);
  return (if p = null then 1 else 0);
end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: {{.*}}argument to `Color.FromNameOrNull`

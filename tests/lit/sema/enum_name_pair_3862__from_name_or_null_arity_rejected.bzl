// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #3862 (§10.4, v0.293; stdlib §9.9, v0.293): §10.4 declares
// `public static method FromNameOrNull(name: string): E?` — ONE parameter, so
// a two-argument call is rejected.

module M;

enum Color Red, Green; end Color;

class C public static method Main(): int
  var p := Color.FromNameOrNull("Red", "Green");
  return (if p = null then 1 else 0);
end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `Color.FromNameOrNull` takes exactly one argument

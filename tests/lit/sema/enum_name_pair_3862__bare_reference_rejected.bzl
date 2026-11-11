// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #3862 (§10.4, v0.293; stdlib §9.9, v0.293): the compiler-provided
// members are CALLABLE statics. A BARE reference to one is not a member
// access the language admits — it falls through to the ordinary "no member"
// reject, exactly as `E.FromUnderlyingOrNull` does (§27.6's method-reference
// posture for a compiler-provided member stays inherited/unspecified).

module M;

enum Color Red, Green; end Color;

class C public static method Main(): int
  var f := Color.NameOf;
  return 0;
end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `NameOf` on type `Color`

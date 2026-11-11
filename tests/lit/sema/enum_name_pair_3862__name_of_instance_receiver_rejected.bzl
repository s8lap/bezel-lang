// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #3862 (§10.4, v0.357): the compiler-provided tier is declared
// `public static method …` — `FromNameOrNull(name: string): E?` and
// `NameOf(value: E): string` — so the RECEIVER must NAME the enum type.
// An INSTANCE receiver (`c.NameOf(c)`) is not a member access the language
// admits: it falls through to the ordinary "no member" reject, exactly as
// it did before the member existed.
//
// The reject must stay a REJECT: a clean rejection, not a silently-accepted
// call that lowers to nothing.

module M;

enum Color Red, Green; end Color;

class C public static method Main(): int
  var c := Color.Red;
  var s := c.NameOf(c);
  return 0;
end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: no member `NameOf` on type `Color`

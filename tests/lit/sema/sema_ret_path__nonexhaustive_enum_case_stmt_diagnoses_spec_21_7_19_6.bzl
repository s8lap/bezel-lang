// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #3044 (§21.7 / §19.6, v0.242) — THE TOTALITY FLIP MUST NOT OVER-GRANT.
// §21.7 condition (i) consumes §19.6's proof: the ENUM bullet grants totality
// only for a match whose "every declared constant has a matching pattern".
// `Blue` has no arm, so the match is NOT statically exhaustive, the no-match
// fall-through is live, and the trailing-exit requirement stands.
//
// This is the guard on the flip's other side — without it, admitting enums to
// the closed-value-space list could be mistaken for admitting every enum
// `case`, whatever its arms.

module M;
enum E Red, Green, Blue; end E;
class C
  public method Classify(e: E): int
    case e of
      Red   => return 0;,
      Green => return 1;
    end;
  end Classify;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: method `Classify` has a return type but does not return a value on every path
// CHECK-SAME: (§21.7)

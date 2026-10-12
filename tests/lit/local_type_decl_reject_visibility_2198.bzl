// RUN: %not %bezelc -c -o %t.o %s 2>&1 | FileCheck %s
//
// §21.10 (v0.334): the `Visibility?` slot must be empty — a local type has
// no access surface. A visibility keyword on a local type declaration is a
// compile error naming the rule. A.5 marks this a compiler-enforced invariant.
//
// REJECT §21.10 visibility restriction (record)
module M2198a;

class C
  public static method M(): int
    public record Point
      public X: int;
    end Point;
    return 0;
  end M;
end C;

// CHECK: a local type may not have a visibility modifier (§21.10)

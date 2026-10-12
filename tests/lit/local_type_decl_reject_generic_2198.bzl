// RUN: %not %bezelc -c -o %t.o %s 2>&1 | FileCheck %s
//
// §21.10 (v0.334): a local type may not have type parameters.
// §21.10: generic params on a class/record/enum are permitted at module
// scope but a local type declaration (grammar A.6 LocalTypeDecl) has no
// type-parameter slot.
//
// REJECT §21.10 generic-param restriction (record with generic param)
module M2198b;

class C
  public static method M(): int
    record Box<T>
      public Value: T;
    end Box;
    return 0;
  end M;
end C;

// CHECK: a local type may not have type parameters (§21.10)

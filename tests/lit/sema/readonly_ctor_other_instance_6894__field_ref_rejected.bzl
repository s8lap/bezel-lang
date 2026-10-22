// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #6894, §22.8 (v0.340): "a constructor of the declaring type may **not**
// write another value's `readonly` storage held in a parameter, local, or
// field"; §23.1 (v0.340): a readonly auto-property is assignable "only through
// `this`". `this.Next.X` is another instance's storage.

module M;
public class Util
  public static method Bump(ref v: int) v := v + 1; end Bump;
  destructor end destructor;
end Util;
public class Holder
  public readonly X: int;
  public owned Next: Holder;
  public constructor()
    this.Next := new Holder();
    Util.Bump(ref this.Next.X);
  end constructor;
  destructor end destructor;
end Holder;
public class App
  public static method Main(): int return 0; end Main;
  destructor end destructor;
end App;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a `ref` argument may not be readonly field `X` outside the declaring class's constructor (§22.3 / §22.8)

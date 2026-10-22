// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #6972, §22.8 (v0.340): "a constructor of the declaring type may **not**
// write another value's `readonly` storage held in a parameter, local, or
// field". An element store into a readonly inline-array field of
// `this.Next` is such a write.

module M;
public class H
  public readonly A: [3]int;
  public owned Next: H;
  public constructor()
    this.Next := new H();
    this.Next.A[0] := 1;
  end constructor;
  destructor end destructor;
end H;
public class App
  public static method Main(): int return 0; end Main;
  destructor end destructor;
end App;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot assign to readonly field `A` outside the declaring class's constructor ({{.*}}§22.8)

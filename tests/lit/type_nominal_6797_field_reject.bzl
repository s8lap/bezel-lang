// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #6797 (stdlib §2.13 "The `Type` class", spec §5.1, v0.340). A field
// initializer of type `Type` is a `Type` location. The int is not a `Type`
// value, so the initializer is rejected at its own location.
module M;
public class F
  public _f: Type := 5;
  // CHECK: :[[@LINE-1]]:{{[0-9]+}}: the initializer for field `_f` has a type that is not implicitly convertible to `Type` (§2.13, §5.1)
  public constructor() end constructor;
  destructor end destructor;
end F;
public class Program
  public static method Main(): int
    return 0;
  end Main;
end Program;

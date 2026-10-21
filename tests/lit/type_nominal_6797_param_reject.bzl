// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #6797 (stdlib §2.13 "The `Type` class", spec §5.1, v0.340). The argument
// to a `Type` parameter must be a `Type` value, and `5` is not one. The call is
// rejected at the argument with the nominal diagnostic.
module M;
public class P
  public static method Take(t: Type): int return 0; end Take;
end P;
public class Program
  public static method Main(): int
    P.Take(5);
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: the argument `t` to call to `Take` has a type that is not implicitly convertible to `Type` (§2.13, §5.1)
    return 0;
  end Main;
end Program;

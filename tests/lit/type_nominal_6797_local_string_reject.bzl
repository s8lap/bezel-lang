// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #6797 (stdlib §2.13 "The `Type` class", spec §5.1, v0.340). A string
// literal is not a `Type` value: stdlib §2.13 names `typeof(T)` and
// `value.GetType()` as the producers of one. Rejected at the initializer with
// the nominal diagnostic. Before the fix this compiled at exit 0.
module M;
public class Program
  public static method Main(): int
    var t: Type := "x";
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: the initializer for `t` has a type that is not implicitly convertible to `Type` (§2.13, §5.1)
    return 0;
  end Main;
end Program;

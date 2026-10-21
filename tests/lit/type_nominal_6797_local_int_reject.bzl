// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #6797 (stdlib §2.13 "The `Type` class", spec §5.1, v0.340). Stdlib §2.13:
// "A `Type` value is obtained from the `typeof(T)` operator (language spec
// §17.10) for a statically named type, or from `value.GetType()` (language spec
// §8.2) for the runtime type of a value." An integer literal is not a `Type`
// value, so the initializer is rejected at its own location with the nominal
// diagnostic. Before the fix `var t: Type := 5;` compiled at exit 0.
module M;
public class Program
  public static method Main(): int
    var t: Type := 5;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: the initializer for `t` has a type that is not implicitly convertible to `Type` (§2.13, §5.1)
    return 0;
  end Main;
end Program;

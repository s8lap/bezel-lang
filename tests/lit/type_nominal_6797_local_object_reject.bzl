// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #6797 (stdlib §2.13 "The `Type` class", spec §5.1, v0.340). An `Object`
// source is the case the Object-source arm (#6023) would admit into a
// declaration-less built-in, so the nominal rule has to sit above that arm. An
// `Object` value is not a `Type` value (stdlib §2.13), so the initializer is
// rejected. Before the fix this compiled at exit 0.
module M;
public class Program
  public static method Main(): int
    var o: Object := 5;
    var t: Type := o;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: the initializer for `t` has a type that is not implicitly convertible to `Type` (§2.13, §5.1)
    return 0;
  end Main;
end Program;

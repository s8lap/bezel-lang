// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix (§15.1.1 v0.307): "The form `[N owned]T` ... T must be a class
// or interface type; value types and runtime-managed reference types are
// not admitted with the `owned` marker." Mirrors rule-corpus case
// s15_1_1_0002 (tests/conformance/rules/cases). Before this fix nothing
// checked the element type of a CONCRETE `[N owned]T` at all — `int`
// compiled cleanly with no diagnostic.

module M;

public class Program
  public static method Foo()
    var x: [3 owned]int;
  end Foo;
end Program;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `[N owned]T` requires T to be a class or interface type
// CHECK-SAME: (§15.1.1)

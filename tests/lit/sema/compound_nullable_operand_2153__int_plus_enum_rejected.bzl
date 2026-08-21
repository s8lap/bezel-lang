// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2153 (§6.6 / §5.1 / §5.4, spec v0.220): `int += enum` — an enum never
// implicitly converts to its underlying integer (§5.1 "never implicit"
// list; §5.4 requires the explicit cast), so no operator `+` result exists
// between `int` and `Priority`. Same Named-RHS gate skip as the record cell.

module M;
enum Priority
  Low, Medium, High;
end Priority;
class C
  public static method Main(): int
    var i: int := 5;
    i += Priority.High;
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: compound assignment `+=` has no valid result: no built-in operator `+` is defined between `int` and enum type `Priority` (§6.2's promotion covers primitive numeric operands only), and `Priority` declares no applicable `operator +` overload (§25.2)

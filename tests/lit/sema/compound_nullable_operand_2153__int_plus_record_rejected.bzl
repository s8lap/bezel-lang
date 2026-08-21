// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2153 (§6.6 / §6.2 / §25.2, spec v0.220): the plain Named-RHS cell —
// `int += record` where the record declares no `operator +`. No built-in
// operator exists between a primitive and a record, so R does not exist and
// the compound form is a compile error (§6.6). Before the fix the check's
// `rTy->kind == Primitive` gate skipped every Named RHS: codegen fed the
// record struct to the integer overflow intrinsic (malformed-IR abort).

module M;
record P(X: int, Y: int)
end P;
class C
  public static method Main(): int
    var i: int := 5;
    var r: P := new P(7, 9);
    i += r;
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: compound assignment `+=` has no valid result: no built-in operator `+` is defined between `int` and record type `P` (§6.2's promotion covers primitive numeric operands only), and `P` declares no applicable `operator +` overload (§25.2)

// §15.4 (#1866) control: the REMEDY named by the rejection compiles cleanly —
// a collection literal targeting a CONCRETE `Array<int>` local, which then
// covariantly (§12.9) initializes an `IEnumerable<int>` local. Only the literal
// bound *directly* to the interface is rejected; naming a concrete backing and
// assigning it is well-formed. Guards against over-rejection (a false positive
// that would reject every concrete collection literal).
//
// NOTE: literal is now Owned per fix #4338 fix-forward; changed `var` to `const`
// to avoid the auto-drop warning (value-typed Array<int> has no disposal obligation).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
class P
  public static method Main(): int
    const xs: Array<int> := [1, 2, 3];
    var v: IEnumerable<int> := xs;
    var total: int := 0;
    for var x in v do total := total + x; end;
    return total;
  end Main;
end P;

// NODIAG-NOT: error
// NODIAG-NOT: §

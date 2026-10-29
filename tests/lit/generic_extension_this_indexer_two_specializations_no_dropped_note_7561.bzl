// fix #7561 (§26.5 "Specificity ordering" worked example, §26.1, v0.350): a
// generic extension on a builtin target whose body reads `this[0]`, instantiated
// at TWO element types, must compile with no "DROPPED" announcement (#2833)
// from either specialization, and the program's stdout must be exact.  The
// method is named `Head` (not `First`) so no builtin claims it.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
// RUN: %bezelc -o %t.exe %s && %t.exe | FileCheck %s
module M;
extension Array<T>
  public method Head(): T
    return this[0];
  end Head;
end;

public static class Program
  public static method Main(): int
    var xs: Array<int> := [1, 2, 3];
    var ys: Array<string> := ["a", "b"];
    Console.WriteLine(xs.Head());
    Console.WriteLine(ys.Head());
    return 0;
  end Main;
end;

// CHECK: 1
// CHECK-NEXT: a
// NODIAG-NOT: warning
// NODIAG-NOT: DROPPED

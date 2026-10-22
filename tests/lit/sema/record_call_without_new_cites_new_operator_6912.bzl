// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §17.7 (v0.340), #6912: calling a top-level record by its type name
// (`Pt(1, 2)`) is not construction. The diagnostic must cite the rule that
// defines construction (§17.7, the `new` operator), not §9.5 (`with`).
module M;
public record Pt(X: int, Y: int)
end Pt;
public class P
  public static method Main(): int
    var p := Pt(1, 2);
    Console.WriteLine(p.X);
    return 0;
  end Main;
end P;

// CHECK: :[[@LINE-6]]:{{[0-9]+}}: `Pt` is constructed with `new Pt(...)`, not by calling the type name (§17.7)

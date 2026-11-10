module Probe;
// REWRITE of the temporary-source shape (corpus case s22_6_2312), form 1: the array literals are bound to locals declared before the holders, so each source outlives its holder. EXPECT: compiles; prints 15. Under A this is admitted with no marker (the constructor mints; `a` is declared before `r1`).
public class Row
  public Items: Array<int>;
  public constructor(a: Array<int>) Items := a; end constructor;
end Row;
public class Program
  public static method Main(): int
    var a: Array<int> := [1, 2];
    var b: Array<int> := [3, 4, 5];
    using var r1 := new Row(a);
    using var r2 := new Row(b);
    var rows: Array<Row> := [r1, r2];
    var t := 0;
    for var v in rows.SelectMany(lambda(r) => r.Items) do t := t + v; end;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;

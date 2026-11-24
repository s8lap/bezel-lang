// bug: B4
// title: `OrderBy`/`OrderByDescending`/`ThenBy` ignore non-`int` keys
// issue: new at r40 (#7780 covers DateTime keys)
// rests-on: stdlib §5: the OrderBy family is a stable sort by the key's IComparable order
// source: data/compiler-bugs.md (second repro block), verbatim below this header
// expect: run
// stdout: 6 1 0.25
// stdout: 6 1 0.25
// stdout: 6
module P;
public choice Sh
  C(r: double),
  R(w: double, h: double);
  public method Area(): double => (case this of C(var r) => r * r, R(var w, var h) => w * h);
end Sh;
public class Program
  public static method Main()
    var xs: Array<Sh> := [Sh.C(1.0), Sh.R(2.0, 3.0), Sh.C(0.5)];
    for var s in xs.OrderByDescending(lambda(s) => s.Area()) do Console.Write($"{s.Area()} "); end;
    Console.WriteLine();
    var ds: Array<double> := [1.0, 6.0, 0.25];
    for var d in ds.OrderByDescending(lambda(d) => d) do Console.Write($"{d} "); end;
    Console.WriteLine();
    Console.WriteLine(xs.OrderByDescending(lambda(s: Sh): double => s.Area()).First().Area());
  end Main;
end Program;

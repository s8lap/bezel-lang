// bug: B7
// title: Untyped comparer lambda comparing `string` members does not sort
// issue: #8387
// rests-on: spec §27.3 inferred parameter types; stdlib §4.4 `Sort(comparer: (T, T) -> int)`, not guaranteed stable (so the third line's three tied 4s may come in any order)
// source: data/compiler-bugs.md, verbatim below this header
// expect: run
// stdout: it4 of4 was4 best1
// stdout: best1 it4 of4 was4
// stdout-re: (?=.*\bit4\b)(?=.*\bof4\b)(?=.*\bwas4\b)(\w+4 ){2}\w+4 best1
module P;
record W(K: string, V: int);
public class Program
  public static method Main()
    var es: Array<W> := [new W("it", 4), new W("was", 4), new W("best", 1), new W("of", 4)];
    es.Sort(lambda(x: W, y: W): int => (if x.V <> y.V then y.V - x.V else x.K.CompareTo(y.K)));
    for var e in es do Console.Write($"{e.K}{e.V} "); end;
    Console.WriteLine();
    var ss: Array<W> := [new W("it", 4), new W("was", 4), new W("best", 1), new W("of", 4)];
    ss.Sort(lambda(x, y) => x.K.CompareTo(y.K));
    for var e in ss do Console.Write($"{e.K}{e.V} "); end;
    Console.WriteLine();
    var ts: Array<W> := [new W("it", 4), new W("was", 4), new W("best", 1), new W("of", 4)];
    ts.Sort(lambda(x, y) => (if x.V <> y.V then y.V - x.V else 0));
    for var e in ts do Console.Write($"{e.K}{e.V} "); end;
    Console.WriteLine();
  end Main;
end Program;

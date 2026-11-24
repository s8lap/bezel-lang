// bug: B3
// title: Untyped key lambda in `GroupBy` produces malformed IR
// issue: #6627
// rests-on: spec §27.3 lambda parameter types are inferred from the target; stdlib §5 `GroupBy<TKey>(key: (T) -> TKey)`
// source: data/compiler-bugs.md, verbatim below this header
// note: The group-element residual is NB17.
// expect: run
// stdout: 2: 2
// stdout: 1: 1
module P;
public class Program
  public static method Main()
    var rows: Array<string> := ["ab", "c", "de"];
    for var g in rows.GroupBy(lambda(e) => e.Length) do
      Console.WriteLine($"{g.Key}: {g.Count()}");
    end;
  end Main;
end Program;

// bug: NB17
// title: B3 residual: an untyped lambda over a group's elements takes the key type
// issue: #8542
// rests-on: stdlib §5 `IGrouping<TKey, T>: IEnumerable<T>`; spec §27.3 lambda parameter inference
// source: r42/data/new-compiler-bugs.md, verbatim below this header
// expect: run
// stdout: a: 4 2 1
// stdout: b: 2 1 1
module P;
record E(Dept: string, Salary: int);
public class Program
  public static method Main()
    var rows: Array<E> := [new E("a", 1), new E("b", 2), new E("a", 3)];
    for var g in rows.GroupBy(lambda(e) => e.Dept) do
      var n := 0;
      for var e in g do n += e.Salary; end;
      Console.WriteLine($"{g.Key}: {n} {g.Count()} {g.Where(lambda(e) => e.Salary > 1).Count()}");
    end;
  end Main;
end Program;

// bug: B4
// title: `OrderBy`/`OrderByDescending`/`ThenBy` ignore non-`int` keys
// issue: new at r40 (#7780 covers DateTime keys)
// rests-on: stdlib §5: the OrderBy family is a stable sort by the key's IComparable order
// source: data/compiler-bugs.md (first repro block), verbatim below this header
// expect: run
// stdout: a
// stdout: b
// stdout: c
// stdout: a,b,c
module P;
public class Program
  public static method Main()
    var xs: Array<string> := ["b", "a", "c"];
    for var e in xs.OrderBy(lambda(e) => e) do
      Console.WriteLine(e);
    end;
    xs.Sort();
    Console.WriteLine(String.Join(",", xs));
  end Main;
end Program;

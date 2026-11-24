// bug: B17
// title: `Select` with an interpolated-string projection is rejected as a leak (contested: #7170, #7215)
// issue: #7170, #7215 (open spec questions: contested)
// rests-on: spec §28.1/§28.9: `string` is runtime-managed, with no ownership and nothing to leak
// source: data/compiler-bugs.md, verbatim below this header
// note: Contested: the expectation is the study's reading of §28.1; #7170/#7215 may rule otherwise.
// expect: run
// stdout: <1> <2>
// stdout: 1 2
module P;
public class Program
  public static method Main()
    var xs: Array<int> := [1, 2];
    Console.WriteLine(String.Join(" ", xs.Select(lambda(x) => $"<{x}>")));
    Console.WriteLine(String.Join(" ", xs.Select(lambda(x) => x.ToString())));
  end Main;
end Program;

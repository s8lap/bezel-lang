// bug: B17
// title: `Select` with an interpolated-string projection is rejected as a leak (ruled 2026-10-27, a compiler bug: #7170, #7215)
// issue: #7170, #7215 (ruled 2026-10-27; implementation trackers)
// rests-on: spec §28.1/§28.9: `string` is runtime-managed, with no ownership and nothing to leak
// source: data/compiler-bugs.md, verbatim below this header
// note: The expectation follows the #7170/#7215 rulings (§28.3 Calls mint clause 7, Bare-type-parameter returns clause 1, K1.6).
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

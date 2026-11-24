// bug: B23
// title: A lazy query consumed inside an interpolation hole draws the must-use warning
// issue: #8308
// rests-on: spec §29.11 (v0.355) consumption point 3, which is §28.3's suppression 4 for the must-use warning
// source: data/compiler-bugs.md, verbatim below this header
// expect: quiet
// stdout: kept: b
// stdout: a
module P;
public class Program
  public static method Main()
    var names: Array<string> := ["a", "b"];
    var q := names.Where(lambda(n) => n <> "a");
    Console.WriteLine($"kept: {String.Join(", ", q)}");   // warns: q implicitly disposed (must-use)
    var r := names.Where(lambda(n) => n <> "b");
    Console.WriteLine(String.Join(", ", r));              // same consumption outside a hole: silent
  end Main;
end Program;

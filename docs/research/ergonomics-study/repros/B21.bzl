// bug: B21
// title: `DateOnly.ToString(format)` not implemented (#2240)
// issue: #2240
// rests-on: stdlib §2.5 `DateOnly.ToString(format)`
// source: data/compiler-bugs.md, verbatim below this header
// expect: run
// stdout: 2026-02-14
module P;
public class Program
  public static method Main()
    var d := new DateOnly(2026, 2, 14);
    Console.WriteLine(d.ToString("yyyy-MM-dd"));
  end Main;
end Program;

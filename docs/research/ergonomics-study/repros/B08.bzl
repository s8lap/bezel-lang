// bug: B8
// title: A narrowed local used as an `if`-expression branch makes the result nullable (ruled 2026-10-21, a compiler bug: #6698)
// issue: #6698 (ruled 2026-10-21; sema tracker)
// rests-on: spec §7.4 (v0.355): inside a narrowing region the variable is narrowed; §20 common type of `string` and `string` is `string`
// source: data/compiler-bugs.md, verbatim below this header
// note: The expectation follows the #6698 ruling (§7.11, §12.7, §22.6): a narrowed read has the narrowed type.
// expect: run
// stdout: 2
// stdout: 5
// stdout: 1
module P;
public class Program
  public static method Main()
    var line := Console.ReadLine();
    while line <> null do
      var hay := (if line.Length > 3 then line.ToLower() else line);
      Console.WriteLine(hay.Length);
      line := Console.ReadLine();
    end;
    var s: string? := "x";
    if s <> null then
      var t := (if true then s else "y");
      Console.WriteLine(t.Length);
    end;
  end Main;
end Program;

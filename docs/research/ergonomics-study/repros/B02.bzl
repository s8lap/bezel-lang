// bug: B2
// title: Must-use warning on trivial-dispose values
// issue: #4418
// rests-on: spec §28.9 (v0.355): no must-use / auto-drop diagnostic attaches to a trivial-dispose value
// source: data/compiler-bugs.md, verbatim below this header
// expect: quiet
// stdout: 1
module P;
public class Program
  private static method Mk(): owned Array<string>
    var a := new Array<string>();
    a.Add("x");
    return a;
  end Mk;
  public static method Main()
    var f := Mk();
    Console.WriteLine(f.Count);
  end Main;
end Program;

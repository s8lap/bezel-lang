// bug: NB2
// title: `string.IsEmpty` is not implemented in codegen
// issue: #2348
// rests-on: stdlib §9.6 and codegen reference §5.9: `pure readonly property IsEmpty: bool`
// source: r42/data/new-compiler-bugs.md, verbatim below this header
// expect: run
// stdout: False
module P;
public class Program
  public static method Main()
    var s := "x";
    Console.WriteLine(s.IsEmpty);
  end Main;
end Program;

// bug: B1
// title: `TextReader.ReadToEnd()` on `Console.In` lowers to nothing
// issue: new at r40; closed #8383
// rests-on: stdlib §7.4 `ReadToEnd(): string` on TextReader; stdlib §2.1 `Console.In: TextReader`
// source: data/compiler-bugs.md, verbatim below this header
// note: The residual string-member shape is NB16.
// expect: quiet
// stdout: 6
module P;
public class Program
  public static method Main()
    var text := Console.In.ReadToEnd();
    Console.WriteLine(text.Length);
  end Main;
end Program;

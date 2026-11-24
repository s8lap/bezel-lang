// bug: NB3
// title: `Exception.ToString()` omits the message
// issue: #3302
// rests-on: spec §31.4 / codegen §5.2: a conforming rendering contains the qualified type name and the Message when non-empty
// source: r42/data/new-compiler-bugs.md, verbatim below this header
// note: The exact rendering is unspecified; each line only has to contain the type name and the message.
// expect: run
// stdout-re: .*InvalidOperationException.*bad state.*
// stdout-re: .*InvalidOperationException.*bad state.*
// stdout: bad state
// stdout-re: .*FormatException.*\bx\b.*
module P;
public class Program
  public static method Main()
    try
      throw new InvalidOperationException("bad state");
    catch
      var ex: Exception => do
        Console.WriteLine(ex.ToString());
        Console.WriteLine($"{ex}");
        Console.WriteLine(ex.Message);
      end
    end;
    var e2 := new FormatException("x");
    Console.WriteLine(e2.ToString());
  end Main;
end Program;

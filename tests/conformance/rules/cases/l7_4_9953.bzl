//! case: l7_4_9953
//! doc: stdlib
//! section: 7.4
//! title: a user subclass of TextReader keeps its own Read and ReadLine overrides
//! rule: User code may derive from `TextReader` and `TextWriter`
//! cites: 7.4, 12.4
//! expect: run
//! output: mine 42
//! notes: Pins #9260 (the Console.In lowering must not capture a user subclass receiver). MyReader
//!   overrides both abstract members; m.ReadLine() answers "mine" and m.Read() answers 42 by
//!   ordinary virtual dispatch.
module Conf;
import System.IO;
public class MyReader : TextReader
  public override method Read(): int
    return 42;
  end Read;
  public override method ReadLine(): string?
    return "mine";
  end ReadLine;
end MyReader;
public class Program
  public static method Main(): int
    using var m := new MyReader();
    var a := m.ReadLine();
    var b := m.Read();
    Console.WriteLine($"{a} {b}");
    return 0;
  end Main;
end Program;

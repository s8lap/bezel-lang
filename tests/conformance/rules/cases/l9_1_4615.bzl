//! case: l9_1_4615
//! doc: stdlib
//! section: 9.1
//! title: long.Parse raises FormatException on padded input
//! rule: no whitespace, no `0x`/`0b` prefix, no digit separators, and any other input raises `FormatException`
//! cites: 9.1
//! expect: run
//! stdout: format
module Conf;

public class Program
  public static method Main()
    try
      _ := long.Parse(" 42");
      Console.WriteLine("none");
    catch
      FormatException => Console.WriteLine("format"),
    end try;
  end Main;
end Program;

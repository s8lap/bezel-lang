//! case: l3_2_0015
//! doc: stdlib
//! section: 3.2
//! title: an Encoding-typed ASCII value raises FormatException above U+007F and on a byte above 0x7F
//! rule: `GetBytes` raises `FormatException` on any code point above `U+007F`; `GetString` raises `FormatException` on any byte above `0x7F`.
//! cites: 3.2
//! expect: run
//! issue: 7028
//! output: fmt1
//!   fmt2

module Conf;

public class Program
  public static method Main()
    var a: Encoding := Encoding.ASCII;
    try
      using var b := a.GetBytes("hé");
      Console.WriteLine("no fault");
    catch
      FormatException => Console.WriteLine("fmt1");
    end;
    using var hi := Encoding.UTF8.GetBytes("é");
    try
      Console.WriteLine(a.GetString(hi));
    catch
      FormatException => Console.WriteLine("fmt2");
    end;
  end Main;
end Program;

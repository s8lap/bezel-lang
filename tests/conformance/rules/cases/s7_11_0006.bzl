//! case: s7_11_0006
//! doc: spec
//! section: 7.11
//! title: narrowing a nullable string first admits ordering
//! rule: `s < t` requires non-nullable operands; narrowing an operand to
//!   non-null admits the ordering operator on it (narrow first, §7.4).
//! cites: 4.6, 7.4
//! expect: run
//! stdout: less
module Conf;

public class Program
  public static method Main()
    var a: string? := "abc";
    var b: string := "abd";
    if a <> null and a < b then
      Console.WriteLine("less");
    end;
  end Main;
end Program;

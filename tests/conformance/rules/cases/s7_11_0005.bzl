//! case: s7_11_0005
//! doc: spec
//! section: 7.11
//! title: string ordering requires non-nullable operands
//! rule: `s < t` performs ordinal lexicographic comparison; ordering
//!   requires non-nullable operands — an operand of static type `string?`
//!   is a compile error.
//! cites: 4.6
//! expect: reject
//! diagnostic: non-nullable operands
module Conf;

public class Program
  public static method Maybe(): string? => "abc";

  public static method Main()
    var a: string? := Maybe();
    var b: string := "abd";
    if a < b then
      Console.WriteLine("less");
    end;
  end Main;
end Program;

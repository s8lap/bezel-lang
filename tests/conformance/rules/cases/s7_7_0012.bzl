//! case: s7_7_0012
//! doc: spec
//! section: 7.7
//! title: a chain of ?? over two string? operands ends at a non-nullable string and prints the terminal
//! rule: "a right-associative chain stays nullable until a non-nullable operand terminates it" ([§7.7]). Here the terminal operand is a non-nullable string literal, so the whole chain is non-nullable, and both null operands fall through to the literal.
//! cites: 7.7
//! expect: run
//! stdout: end
//! issue: 6214
module Conf;

public class Program
  public static method Main()
    var s: string? := null;
    var t: string? := null;
    Console.WriteLine(s ?? t ?? "end");
  end Main;
end Program;

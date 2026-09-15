//! case: s16_1_0003
//! doc: spec
//! section: 16.1
//! title: mixing bitwise AND with a range operand requires explicit parenthesization
//! rule: Range (`..`, `..<`) with bitwise (`&`, `|`, `^`) is a mandatory-
//!   parenthesization mix: "`mask & 0..n` is a compile error; write `(mask & 0)..n`
//!   or `mask & (0..n)` to disambiguate."
//! cites: 16.1, 17.2
//! expect: reject
//! diagnostic: parenthesization
module Conf;

public class Program
  public static method Main()
    var mask: int := 12;
    var n: int := 5;
    var r := mask & 0..n;
  end Main;
end Program;

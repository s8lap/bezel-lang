//! case: s9_1_0001
//! doc: spec
//! section: 9.1
//! title: a positional record generates a primary constructor and read-only properties
//! rule: For each positional record, the compiler generates a primary constructor
//!   whose parameters are the positional fields, in declaration order, and public
//!   read-only properties, one per positional field, with the same names.
//! expect: accept
module Conf;

public record Point(X: int, Y: int);

public class Program
  public static method Describe(): int
    var p := new Point(3, 4);
    return p.X + p.Y;
  end Describe;
end Program;

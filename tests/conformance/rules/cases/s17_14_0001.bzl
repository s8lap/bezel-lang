//! case: s17_14_0001
//! doc: spec
//! section: 17.14
//! title: constant integer arithmetic over a primitive-wrapper constant folds
//! rule: "`const H := int.MaxValue / 2;` // 1073741823 : int" — a reference
//!   to a compiler-provided constant of a primitive wrapper type, combined
//!   with the constant binary arithmetic operators, is a constant
//!   expression.
//! cites: 17.14
//! expect: run
//! stdout: 1073741823
module Conf;

public class Program
  public static method Main()
    const h := int.MaxValue / 2;
    Console.WriteLine(h);
  end Main;
end Program;

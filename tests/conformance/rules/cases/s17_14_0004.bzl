//! case: s17_14_0004
//! doc: spec
//! section: 17.14
//! title: a constant if expression with constant condition and branches folds
//! rule: "the `if` expression `(if c then a else b)` whose condition and both
//!   branches are constant expressions [is a constant expression] ... `const
//!   Cap := (if Flags > 0 then 64 else 32);`"
//! cites: 17.14, 20
//! expect: run
//! stdout: 64
module Conf;

public class Program
  public static method Main()
    const flags: long := (1 << 10) | (1 << 3);
    const cap := (if flags > 0 then 64 else 32);
    Console.WriteLine(cap);
  end Main;
end Program;

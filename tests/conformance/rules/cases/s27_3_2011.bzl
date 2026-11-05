//! case: s27_3_2011
//! doc: spec
//! section: 27.3
//! title: a lambda returning a new record value at a record slot is
//!   admitted
//! rule: its inferred return type must equal the target's return slot or
//!   implicitly convert to it
//! cites: 5.1
//! expect: run
//! stdout: 3
module Conf;

record Point(X: int, Y: int);

public class Program
  public static method Main()
    var f: (int) -> Point := lambda(x) => new Point(x, x + 1);
    Console.WriteLine(f(2).Y);
  end Main;
end Program;

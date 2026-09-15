//! case: s17_11_0007
//! doc: spec
//! section: 17.11
//! title: default of a record whose fields are all defaultable zeroes every field
//! rule: "Records all of whose declared fields are defaultable and which are
//!   not constructor-only: the record value with each field at its zero
//!   value."
//! cites: 17.11, 9.9
//! expect: run
//! stdout: True
module Conf;

public record Point(X: int, Y: int);

public class Program
  public static method Main()
    var p := default(Point);
    Console.WriteLine(p = new Point(0, 0));
  end Main;
end Program;

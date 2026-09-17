//! case: s9_5_0002
//! doc: spec
//! section: 9.5
//! title: with applies to a freshly constructed record value
//! rule: new Point(1, 2) with(Y := 9) has a record-typed source and is an
//!   ordinary record with applied to the freshly-constructed value; the
//!   discriminator is the source expression's type kind, never the presence of
//!   new.
//! expect: run
//! stdout: 1,9
module Conf;

public record Point(X: int, Y: int);

public class Program
  public static method Main()
    var p := new Point(1, 2) with(Y := 9);
    Console.WriteLine($"{p.X},{p.Y}");
  end Main;
end Program;

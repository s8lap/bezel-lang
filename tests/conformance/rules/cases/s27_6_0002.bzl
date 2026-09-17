//! case: s27_6_0002
//! doc: spec
//! section: 27.6
//! title: a property reference produces the getter, typed (Receiver) -> PropertyType
//! rule: Method references work for properties; a property reference produces
//!   the getter, and its type is `(Receiver) -> PropertyType` for instance
//!   properties.
//! cites: 27.6
//! expect: run
//! stdout: 5
module Conf;

public class Program
  public static method Main()
    var length: (string) -> int := string.Length;
    Console.WriteLine($"{length("hello")}");
  end Main;
end Program;

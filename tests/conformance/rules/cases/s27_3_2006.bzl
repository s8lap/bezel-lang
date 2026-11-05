//! case: s27_3_2006
//! doc: spec
//! section: 27.3
//! title: a lambda returning a derived-class value at a base-class slot is
//!   admitted
//! rule: its inferred return type must equal the target's return slot or
//!   implicitly convert to it
//! cites: 5.1
//! expect: run
//! stdout: 1
module Conf;

class Base
  public method V(): int => 1;
end Base;

class Derived: Base
  public method W(): int => 2;
end Derived;

public class Program
  public static method Main()
    using var d := new Derived();
    var f: (Derived) -> Base := lambda(x) => x;
    Console.WriteLine(f(d).V());
  end Main;
end Program;

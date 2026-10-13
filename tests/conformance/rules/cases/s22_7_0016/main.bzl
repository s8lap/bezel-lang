//! case: s22_7_0016
//! doc: spec
//! section: 22.7
//! title: an imported base's static constructor runs before its constructor body at a derived construction
//! rule: A static constructor runs once before the type's **first use**: ...
//!   a run of an instance constructor (the base's too, when a derived class's
//!   construction chains to it, §12.8), or a call of an instance member —
//!   whatever module declares the type
//! cites: 22.7, 22.13, 2.8.3
//! expect: run
//! output: base static
//!   base ctor
//!   derived ctor
//! issue: 5908
module App;
import Lib;

public class Derived: Base
  public constructor()
    Console.WriteLine("derived ctor");
  end constructor;
end Derived;

public class Program
  public static method Main()
    using var d := new Derived();
  end Main;
end Program;

//! case: s22_7_0014
//! doc: spec
//! section: 22.7
//! title: a derived construction is a first use of the base, whose static constructor runs first
//! rule: A static constructor runs once before the type's **first use**: the
//!   first time any code of the type runs or any of its static state is read
//!   — ... a run of an instance constructor (the base's too, when a derived
//!   class's construction chains to it, §12.8)
//! cites: 22.7, 22.13, 12.8
//! expect: run
//! output: base static
//!   base ctor
//!   derived ctor
module Conf;

public class Base
  static constructor()
    Console.WriteLine("base static");
  end constructor;

  public constructor()
    Console.WriteLine("base ctor");
  end constructor;
end Base;

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

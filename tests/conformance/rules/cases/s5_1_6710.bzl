//! case: s5_1_6710
//! doc: spec
//! section: 5.1
//! title: a type parameter converts to the class its constraint names
//! rule: **a type parameter to an interface or class its constraints guarantee
//! cites: 14.2
//! expect: run
//! stdout: 7
module Conf;

public class Base
  public constructor() end constructor;
  public method Id(): int => 7;
end Base;

public class Derived: Base
  public constructor() base(); end constructor;
end Derived;

public class Program
  static method Show<T>(x: T): int where T: Base
    var b: Base := x;
    return b.Id();
  end Show;
  public static method Main()
    using var d := new Derived();
    Console.WriteLine(Program.Show(d));
  end Main;
end Program;

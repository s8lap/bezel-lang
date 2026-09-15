//! case: s11_7_0010
//! doc: spec
//! section: 11.7
//! title: a static constructor is the one constructor a static class may declare
//! rule: A static class may not have constructors *other than a static
//!   constructor*; the static constructor runs after all static field
//!   initializers complete (§22.13).
//! cites: 11.7, 22.13
//! expect: run
//! stdout: 42
module Conf;

public static class Config
  public static Limit: int := 0;

  static constructor()
    Limit := 42;
  end constructor;
end Config;

public class Program
  public static method Main()
    Console.WriteLine(Config.Limit);
  end Main;
end Program;

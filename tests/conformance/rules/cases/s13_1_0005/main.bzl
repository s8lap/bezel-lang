//! case: s13_1_0005
//! doc: spec
//! section: 13.1
//! title: a protected member is reachable from a derived class in another module
//! rule: `protected` — Visible to the declaring class and all classes derived
//!   from it. §13.1: a derived class in a different module sees `protected`
//!   even though it does not see `internal` — that is the whole point of the
//!   level.
//! cites: 13.1
//! expect: run
//! stdout: 42
module App;
import Lib;

public class Derived: Base
  public constructor(secret: int)
    base(secret);
  end constructor;

  public method Reveal(): int => _secret;
end Derived;

public class Program
  public static method Main()
    using var d := new Derived(42);
    Console.WriteLine(d.Reveal());
  end Main;
end Program;

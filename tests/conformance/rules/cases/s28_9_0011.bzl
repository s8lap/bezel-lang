//! case: s28_9_0011
//! doc: spec
//! section: 28.9
//! title: a trivial-dispose declared type over a destructor-bearing production is not exempt
//! rule: The exemption's classification is read from the produced type where
//!   the drop site's value has a statically-known producer: a trivial-dispose
//!   declared type standing over a destructor-bearing production —
//!   `var b: Base := new Derived();` at a trivial-dispose `Base` — is not
//!   exempt (§28.9).
//! cites: 28.9, 28.3, 3.6
//! expect: warn
//! diagnostic: must use
module Conf;

public class Base
  public constructor()
  end constructor;

  public method Ping()
    Console.WriteLine("ping");
  end Ping;
end Base;

public class Derived: Base
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("released");
  end destructor;
end Derived;

public class Program
  public static method Run()
    var b: Base := new Derived();
    b.Ping();
  end Run;
end Program;

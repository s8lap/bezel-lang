//! case: s28_9_3601
//! doc: spec
//! section: 28.9
//! title: a move initializer carries its source's produced type into the trivial-dispose exemption key
//! rule: A marked transfer `move x` is no producer of its own and carries **x's key**: x's produced type
//!   where x's value at the transfer has one statically-known producer … and x's declared type otherwise,
//!   an `owned` parameter keying on its declared type; the destination's declared type never decides it,
//!   so `var d := new Derived(); var b: Base := move d;` is not exempt.
//! cites: 28.9, 28.3
//! expect: warn
//! diagnostic: must use
//! issue: 8716
//! notes: `b` is dropped undischarged. Its key is `d`'s produced type `Derived`, which has a destructor,
//!   so the must-use warning is owed although `Base` is trivial-dispose.
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
    var d := new Derived();
    var b: Base := move d;
    b.Ping();
  end Run;
end Program;

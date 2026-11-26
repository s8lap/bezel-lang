//! case: s28_9_3603
//! doc: spec
//! section: 28.9
//! title: a move of an owned parameter keys the exemption on the parameter's declared type
//! rule: A marked transfer `move x` is no producer of its own and carries **x's key**: x's produced type
//!   where x's value at the transfer has one statically-known producer … and x's declared type otherwise,
//!   an `owned` parameter keying on its declared type; the destination's declared type never decides it,
//!   so `var d := new Derived(); var b: Base := move d;` is not exempt.
//! cites: 28.9, 28.3, 22.3
//! expect: warn
//! diagnostic: must use
//! issue: 8716
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
  public static method Run(owned p: Derived)
    var b: Base := move p;
    b.Ping();
  end Run;
end Program;

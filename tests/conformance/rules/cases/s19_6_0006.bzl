//! case: s19_6_0006
//! doc: spec
//! section: 19.6
//! title: a generic closed hierarchy's leaf coverage is not attempted
//! rule: When the closed hierarchy's root or any leaf is generic, leaf
//!   coverage is not attempted; such a case is completable only by an
//!   irrefutable arm and draws the non-exhaustiveness warning without one.
//! cites: 19.6
//! expect: warn
//! diagnostic: generic
module Conf;

public closed class Box<T>
  public constructor()
  end constructor;
end Box;

public sealed class Full<T>: Box<T>
  public constructor()
  end constructor;
end Full;

public sealed class Empty<T>: Box<T>
  public constructor()
  end constructor;
end Empty;

public class Program
  public static method Describe<T>(b: Box<T>): string => (case b of
    var f: Full<T>  => "full",
    var e: Empty<T> => "empty");

  public static method Main()
    var b: Box<int> := new Full<int>();
    Console.WriteLine(Describe(b));
    _ := b;
  end Main;
end Program;

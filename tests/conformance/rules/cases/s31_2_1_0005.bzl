//! case: s31_2_1_0005
//! doc: spec
//! section: 31.2.1
//! title: a catch arm naming a non-Exception-derived class is rejected
//! rule: With every throwable `Exception`-derived and inheritance single, an
//!   arm naming any other class type is provably dead — no thrown instance
//!   can ever match — and is a compile error naming this rule.
//! cites: 31.2.1
//! expect: reject
//! diagnostic: dead
module Conf;

public class NotAnException
  public constructor()
  end constructor;
end NotAnException;

public class Program
  public static method DoWork()
  end DoWork;

  public static method Main()
    try
      DoWork();
    catch
      NotAnException => Console.WriteLine("unreachable"),
    end try;
  end Main;
end Program;

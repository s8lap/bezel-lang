//! case: s22_7_0015
//! doc: spec
//! section: 22.7
//! title: constructing a class derived from a permanently failed type raises TypeInitializationException
//! rule: Every subsequent use of the type — an access to a static member
//!   (field read, field write, method call, property read or write, indexer),
//!   an instance construction of the type or of a class derived from it, and
//!   a call of an instance member — raises `TypeInitializationException`
//! cites: 22.7, 12.8
//! expect: run
//! output: base failed
//!   derived failed
//! issue: 5908
module Conf;

public class Base
  static constructor()
    throw new InvalidOperationException("boom");
  end constructor;

  public constructor()
  end constructor;
end Base;

public class Derived: Base
  public constructor()
  end constructor;
end Derived;

public class Program
  public static method Main()
    try
      using var b := new Base();
    catch
      TypeInitializationException => Console.WriteLine("base failed")
    end try;
    try
      using var d := new Derived();
    catch
      TypeInitializationException => Console.WriteLine("derived failed")
    end try;
  end Main;
end Program;

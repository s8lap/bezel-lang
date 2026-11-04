//! case: s9_8_0047
//! doc: spec
//! section: 9.8
//! title: a class's synthesized Serialize raises for a derived instance reached through the base
//! rule: **The run-time type check.** The synthesized `Serialize` of a class
//!   `C` with no `polymorphic` registry of its own writes `C`'s form, and
//!   raises `SerializationException` when the receiver's run-time type is a
//!   class derived from `C`, because that form omits the derived class's own
//!   components and reads back as a `C`. … It holds however the member is
//!   reached: as a component's `Serialize`, through a `C`-typed receiver, or as
//!   the member a derived class inherits.
//! cites: 9.8, 12.8
//! expect: run
//! output: ok
//!   SER
module Conf;

import System.Serialization;

public class Base: ISerializable
  public V: int := 1;

  public constructor()
  end constructor;
end Base;

public class Derived: Base
  public constructor()
  end constructor;
end Derived;

public class Program
  public static method Main()
    using var b := new Base();
    using var d := new Derived();
    Write(b);
    Write(d);
  end Main;

  private static method Write(x: Base)
    try
      using var s := new JsonSerializer();
      x.Serialize(s);
      Console.WriteLine("ok");
    catch
      SerializationException => Console.WriteLine("SER")
    end;
  end Write;
end Program;

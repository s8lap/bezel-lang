//! case: s28_0001
//! doc: spec
//! section: 28
//! title: an interface-typed value may be discarded and is disposed by dispatch at end of statement
//! rule: The discard form `_ := expr;` disposes the value at the end of the
//!   discard statement for any class or interface type; at an interface-typed
//!   location the discard is permitted and disposes via runtime dispatch, so a
//!   future non-trivial implementation poses no cross-module hazard (§28's
//!   summary of disposal obligations by type kind).
//! cites: 28, 17.13, 28.9
//! expect: run
//! output: disposed
//!   after
module Conf;

public interface IThing
  method Ping();
end IThing;

public class Thing: IThing
  public constructor()
  end constructor;

  public method Ping()
    Console.WriteLine("ping");
  end Ping;

  destructor
    Console.WriteLine("disposed");
  end destructor;
end Thing;

public class Factory
  public static method Make(): owned IThing
    var t := new Thing();
    return t;
  end Make;
end Factory;

public class Program
  public static method Main()
    _ := Factory.Make();
    Console.WriteLine("after");
  end Main;
end Program;

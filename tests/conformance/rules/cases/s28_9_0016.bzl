//! case: s28_9_0016
//! doc: spec
//! section: 28.9
//! title: an interface-typed owned binding is never exempt from the must-use warning
//! rule: An interface-typed binding is never exempt from the must-use warning,
//!   its conformance set being open across modules (§28.9's
//!   possibly-non-trivial-dispose reading), whatever the concrete class behind
//!   it does (§28.9, §28.3).
//! cites: 28.9, 28.3, 21.9
//! expect: warn
//! diagnostic: must use
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
end Thing;

public class Factory
  public static method Make(): owned IThing
    var t := new Thing();
    return t;
  end Make;
end Factory;

public class Program
  public static method Run()
    var t := Factory.Make();
    t.Ping();
  end Run;
end Program;

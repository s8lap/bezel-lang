//! case: s17_7_0007
//! doc: spec
//! section: 17.7
//! title: the new() shorthand is admitted at a return statement under a declared return type
//! rule: "The shorthand is permitted at the written-declared-type positions
//!   ... and the `return e;` statement of (v) above: field initializers,
//!   typed local-variable declarations, default parameter values, and return
//!   statements (when the method has a declared return type)."
//! cites: 17.7
//! expect: run
//! stdout: made
module Conf;

public class Config
  public constructor()
  end constructor;
end Config;

public class Factory
  public static method Make(): owned Config
    return new();
  end Make;
end Factory;

public class Program
  public static method Main()
    using var c := Factory.Make();
    Console.WriteLine("made");
  end Main;
end Program;

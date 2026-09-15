//! case: s17_12_0004
//! doc: spec
//! section: 17.12
//! title: throw has the bottom type — assignable to any expected type
//! rule: "Both forms have the bottom type — assignable to any expected type —
//!   because control transfers to the matching handler before any value is
//!   produced."
//! cites: 17.12
//! expect: run
//! stdout: 5
module Conf;

public class Program
  public static method Main()
    var c := true;
    var v: int := (if c then 5 else throw new InvalidOperationException("unreached"));
    Console.WriteLine(v);
  end Main;
end Program;

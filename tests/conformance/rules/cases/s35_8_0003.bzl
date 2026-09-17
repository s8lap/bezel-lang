//! case: s35_8_0003
//! doc: spec
//! section: 35.8
//! title: calling Dispose on a choice value is rejected
//! rule: Dispose stays off the surface everywhere: choices do not implement
//!   IDisposable — an owning choice's disposal obligation is discharged by its
//!   holder, never by calling Dispose() on it.
//! expect: reject
//! diagnostic: Dispose
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public choice Outcome
  Fetched(owned w: Widget),
  Failed;
end Outcome;

public class Program
  public static method Main()
    var o := Outcome.Fetched(new Widget());
    o.Dispose();   // ERROR: choices do not implement IDisposable
  end Main;
end Program;

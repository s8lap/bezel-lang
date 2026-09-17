//! case: s35_9_0001
//! doc: spec
//! section: 35.9
//! title: assigning null into an owned-state C? location disposes the displaced value
//! rule: For an owning C, x := null; on an owned C? location is drop-on-overwrite — the
//!   displaced value's payloads are disposed.
//! cites: 35.9, 35.4, 28.7
//! expect: run
//! output: widget disposed
//!   nulled
module Conf;

public class Widget
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("widget disposed");
  end destructor;
end Widget;

public choice Outcome
  Fetched(owned w: Widget),
  Failed;
end Outcome;

public class Program
  public static method Main()
    var o: Outcome? := Outcome.Fetched(new Widget());
    o := null;             // drop-on-overwrite: the displaced value's payloads are disposed here
    Console.WriteLine("nulled");
  end Main;
end Program;

//! case: l4_13_0004
//! doc: stdlib
//! section: 4.13
//! title: an undischarged owned for-loop variable over an owning source auto-drops with a warning
//! rule: A for var x in src do ... end where src is an IOwningEnumerable<T>
//!   produces owned iteration variables; where the body does not discharge
//!   x, the affine auto-drop disposes it at the iteration's scope end with
//!   the plain-var must-use warning.
//! cites: 4.13, 28.3
//! expect: warn
//! diagnostic: must-use
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    using var pool := new OwningArray<Widget>();
    pool.Add(new Widget());
    for var w in pool.Drain() do
      Console.WriteLine("iterating");
    end;
  end Main;
end Program;

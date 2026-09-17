//! case: s29_2_0009
//! doc: spec
//! section: 29.2
//! title: iterating Drain() yields an owned element that the body discharges via move
//! rule: The iteration variable `x` is a plain owned local ... the user must
//!   be able to discharge `x` by moving it into another owned slot, e.g.
//!   `using var c := move x` for scope-bound adoption.
//! cites: 29.2
//! expect: run
//! stdout: 0
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    using var pool := new OwningArray<Widget>();
    pool.Add(new Widget());
    pool.Add(new Widget());
    for var conn in pool.Drain() do
      using var c := move conn;
    end;
    Console.WriteLine(pool.Count);
  end Main;
end Program;

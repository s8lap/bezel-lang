//! case: l4_13_0005
//! doc: stdlib
//! section: 4.13
//! title: disposing a drain-view frame does not dispose the collection; partial iteration leaves the remainder
//! rule: Every owning collection's Drain() mints a fresh drain-view frame --
//!   a caller-owned enumerable over the collection, never the collection
//!   re-exposed -- so the frame's disposal at loop end never disposes the
//!   collection, and partial iteration leaves the undrained remainder in place.
//! cites: 4.13, 4.5
//! expect: run
//! stdout: 1
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
    do
      using var view := pool.Drain();
      using var it := view.GetOwningEnumerator();
      _ := it.MoveNext();
      using var w := it.TakeCurrent();
    end;
    Console.WriteLine(pool.Count);
  end Main;
end Program;

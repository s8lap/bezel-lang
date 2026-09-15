//! case: s28_3_0022
//! doc: spec
//! section: 28.3
//! title: using var y := x, unmarked, is a borrow-pin that assumes no obligation
//! rule: An unmarked `using var y := x;` over a named owned binding does not adopt: no
//!   obligation is assumed, `x` keeps it and stays readable, and the pin extends
//!   `x`'s required liveness to the block's end.
//! cites: 28.10
//! expect: run
//! output: pinned
//!   disposed
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Ping()
    Console.WriteLine("pinned");
  end Ping;

  destructor
    Console.WriteLine("disposed");
  end destructor;
end Widget;

public class Program
  public static method Main()
    using var w := new Widget();
    using var pin := w;
    w.Ping();
  end Main;
end Program;

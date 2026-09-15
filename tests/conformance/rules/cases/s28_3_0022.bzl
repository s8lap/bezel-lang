//! case: s28_3_0022
//! doc: spec
//! section: 28.3
//! title: using var y := x, unmarked, is a borrow-pin that assumes no obligation
//! rule: An unmarked `using var y := x;` over a named owned binding does not adopt: no
//!   obligation is assumed, and the pin extends the source's required liveness to
//!   the block's end.
//! expect: run
//! stdout: pinned
module Conf;

public class Widget
  destructor
    Console.WriteLine("disposed");
  end destructor;
end Widget;

public class Program
  public static method Main()
    using var w := new Widget();
    using var pin := w;
    Console.WriteLine("pinned");
  end Main;
end Program;

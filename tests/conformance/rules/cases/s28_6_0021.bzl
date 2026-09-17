//! case: s28_6_0021
//! doc: spec
//! section: 28.6
//! title: reading a captured owned binding inside a closure is admitted
//! rule: Inside a closure a captured owned binding is uniformly in borrow
//!   state: reads borrow, while assignment, a marked transfer, an explicit
//!   `Dispose()` or a discard of it are compile errors.
//! cites: 27.5, 28.3, 28.6
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Demo()
    using var w := new Widget();
    var show: () -> () := lambda() => Console.WriteLine(w);
    show();
  end Demo;
end Program;

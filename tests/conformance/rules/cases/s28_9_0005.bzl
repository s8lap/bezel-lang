//! case: s28_9_0005
//! doc: spec
//! section: 28.9
//! title: a closure may read a captured owned local while the original binding retains the obligation
//! rule: The capture is a borrow whose source is the local's enclosing
//!   scope, borrow-state throughout, and the original `using var` (or other
//!   owned location) retains the disposal obligation.
//! cites: 28.9, 28.3
//! expect: run
//! output: pinged
//!   dispose widget
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Ping()
    Console.WriteLine("pinged");
  end Ping;

  destructor
    Console.WriteLine("dispose widget");
  end destructor;
end Widget;

public class Program
  public static method Main()
    using var w := new Widget();
    var pinger := lambda() => w.Ping();
    pinger();
  end Main;
end Program;

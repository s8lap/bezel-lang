//! case: s28_6_0021
//! doc: spec
//! section: 28.6
//! title: a closure that does not touch an owned binding leaves it disposable
//! rule: A closure cannot consume, reassign, or dispose a captured owned
//!   binding; a closure that captures nothing owned imposes no such
//!   restriction, and the binding may still be disposed outside the closure.
//! cites: 27.5, 28.3, 28.6
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Demo()
    var w := new Widget();
    var show: () -> () := lambda() => Console.WriteLine("show");
    show();
    w.Dispose();
  end Demo;
end Program;

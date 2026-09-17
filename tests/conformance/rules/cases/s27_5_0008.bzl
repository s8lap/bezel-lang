//! case: s27_5_0008
//! doc: spec
//! section: 27.5
//! title: a closure that captures a borrow may be passed to a plain function-typed parameter
//! rule: A capture-bearing closure may be passed as an argument to a
//!   function-typed parameter — a plain parameter is call-bounded, and the
//!   callee's use is bounded by the call (§28.3, v0.298).
//! cites: 27.5, 28.3, 28.14
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Greet()
    Console.WriteLine("hi");
  end Greet;
end Widget;

public class Runner
  public static method Invoke(cb: () -> ())
    cb();
  end Invoke;
end Runner;

public class Program
  public method Demo(w: Widget)
    Runner.Invoke(lambda() => w.Greet());
  end Demo;
end Program;

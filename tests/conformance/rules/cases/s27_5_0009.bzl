//! case: s27_5_0009
//! doc: spec
//! section: 27.5
//! title: reassigning a plain function-typed local from a capturing closure is legal
//! rule: Reassigning a plain function-typed local is fine — each assignment is
//!   checked on its own (§28.3, v0.298).
//! cites: 27.5, 28.3
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Greet()
    Console.WriteLine("hi");
  end Greet;
end Widget;

public class Program
  public method Demo(w: Widget)
    var cb: () -> () := lambda() => Console.WriteLine("first");
    cb := lambda() => w.Greet();
    cb();
  end Demo;
end Program;

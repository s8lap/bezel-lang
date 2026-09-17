//! case: s17_15_0001
//! doc: spec
//! section: 17.15
//! title: the binding atom binds a name and evaluates the body against it
//! rule: "`(var name := initializer; body)` ... the body is one full
//!   expression, and the atom's value, type, and ownership classification
//!   are the body's." Worked example: `var area := (var w :=
//!   bounds.Width(); w * w);`
//! cites: 17.15
//! expect: run
//! stdout: 16
module Conf;

public class Bounds
  private readonly _w: double;

  public constructor(w: double)
    _w := w;
  end constructor;

  public method Width(): double => _w;
end Bounds;

public class Program
  public static method Main()
    using var bounds := new Bounds(4.0);
    var area := (var w := bounds.Width(); w * w);
    Console.WriteLine(area);
  end Main;
end Program;

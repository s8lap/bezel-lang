//! case: s17_13_0034
//! doc: spec
//! section: 17.13
//! title: the anonymous owning binding is a permitted form and draws no diagnostic
//! rule: `using var _ := expression;` is the one `_` form that owns storage — it
//!   binds the value to an unnamed scope-bound local, with scope-end disposal and
//!   every other §28.10 rule of a named `using var`.
//! cites: 17.13, 21.9, 28.10
//! expect: quiet
module Conf;

public class Gauge
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("gauge closed");
  end destructor;
end Gauge;

public class Meter
  public method Open(): owned Gauge
    return new Gauge();
  end Open;

  public method Probe()
    using var _ := Open();
  end Probe;
end Meter;

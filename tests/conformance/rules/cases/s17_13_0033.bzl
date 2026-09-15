//! case: s17_13_0033
//! doc: spec
//! section: 17.13
//! title: discarding an owned result is a permitted form and draws no diagnostic
//! rule: An owned result is disposed at the end of the discard statement ... the
//!   explicit "I want this owned result disposed now" form, useful when an
//!   `owned`-returning method's result is not needed.
//! cites: 17.13, 21.9, 28.3
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
    _ := Open();
  end Probe;
end Meter;

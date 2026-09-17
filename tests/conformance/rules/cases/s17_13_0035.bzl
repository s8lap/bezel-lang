//! case: s17_13_0035
//! doc: spec
//! section: 17.13
//! title: discarding a borrowed local is purely cosmetic and draws no diagnostic
//! rule: A discard whose operand is in **borrow state** disposes nothing: the
//!   discard is purely cosmetic (it silences the §21.9 unused-result warning where
//!   one would fire), and the value's owner keeps the obligation.
//! cites: 17.13, 28.3
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
    using var g := Open();
    var b := g;
    _ := b;
  end Probe;
end Meter;

//! case: s21_9_0026
//! doc: spec
//! section: 21.9
//! title: a bare-statement call returning a non-trivial-dispose class is a hard error
//! rule: For non-trivial-dispose class types ... the warning is replaced by a hard
//!   error directing the caller to the discard `_ := expression;` ... `OpenLog();`
//!   is rejected, and `_ := OpenLog();` is the remedy.
//! cites: 21.9, 17.13, 28.9
//! expect: reject
//! diagnostic: discard
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
    Open();
  end Probe;
end Meter;

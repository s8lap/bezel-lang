//! case: s30_1_0017
//! doc: spec
//! section: 30.1
//! title: an undischarged owned async parameter carries the must-use warning
//! rule: An `owned` parameter never discharged by the body carries the
//!   §28.3 must-use warning at its declaration; `using var c := move conn;`
//!   on entry is the warning-free way to declare frame-lifetime intent. The
//!   class declares a destructor so it is not trivial-dispose (§28.9), whose
//!   ceremony exemption is over the obligation at every holder form — an
//!   undischarged `owned` async parameter among them — and would otherwise
//!   free it silently with no warning.
//! cites: 30.1, 28.3, 28.9, C.3
//! expect: warn
//! diagnostic: must-use
module Conf;

public class Widget
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("closed");
  end destructor;
end Widget;

public class Sink
  public async method ConsumeAsync(owned w: Widget): Task
    await Task.Delay(0);
  end ConsumeAsync;
end Sink;

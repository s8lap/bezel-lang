//! case: s28_2_3603
//! doc: spec
//! section: 28.2
//! title: a holder read through a non-owned field is rejected conservatively after the variable's disposal
//! rule: A member-read holder is keyed on its source set like every other: … through a field that is not
//!   `owned` the referent may still live, and the rejection is then conservative — the analysis reads no
//!   field ownership along the path; the remedy is to borrow from the referent's own binding.
//! cites: 28.2, 28.3.4
//! expect: reject
//! diagnostic: dominat
//! issue: 8708
//! notes: `d` outlives `h`, so the program would not fault; the rule rejects it anyway (the stated cost,
//!   proposal 0205 §9 item 3). The remedy is `d.Ping()`.
module Conf;

public class D
  private _n: int;
  public constructor(n: int) _n := n; end constructor;
  public method Ping(): int => _n;
end D;

public class H
  public Ref: D;
  public constructor(d: D) Ref := d; end constructor;
end H;

public class Program
  public static method Main()
    using var d := new D(7);
    var h := new H(d);
    var v := h.Ref;
    h.Dispose();
    Console.WriteLine(v.Ping());
  end Main;
end Program;

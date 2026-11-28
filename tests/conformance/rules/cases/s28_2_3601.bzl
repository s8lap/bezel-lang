//! case: s28_2_3601
//! doc: spec
//! section: 28.2
//! title: a local bound from an owned field read of a variable is a holder its dominating disposal kills
//! rule: or a local bound from a member read or call through the variable or through such a holder
//!   (`var v := a.F;`, `var v := a.Get();`, a borrowing-match binding over `a.F`), which §28.3's
//!   receiver-sourced rows source at the variable's value whether or not the field is `owned` … a use
//!   of the holder — a read or member access through it … — is the same compile error
//! cites: 28.2, 28.3.4
//! expect: reject
//! diagnostic: dominat
//! issue: 8708
//! notes: The issue's repro. Accepting twin s28_2_3604 (the read moves before the kill).
module Conf;

public class D
  private _n: int;
  public constructor(n: int) _n := n; end constructor;
  public method Ping(): int => _n;
end D;

public class H
  public owned Inner: D;
  public constructor(n: int) Inner := new D(n); end constructor;
end H;

public class Program
  public static method Run(): int
    var h := new H(7);
    var v := h.Inner;
    h.Dispose();
    return v.Ping();
  end Run;
  public static method Main()
    Console.WriteLine(Program.Run());
  end Main;
end Program;

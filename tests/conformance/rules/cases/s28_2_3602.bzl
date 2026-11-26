//! case: s28_2_3602
//! doc: spec
//! section: 28.2
//! title: a local bound from a borrow-returning call is a holder the dominating disposal kills
//! rule: or a local bound from a member read or call through the variable … (`var v := a.Get();`) … a
//!   use of the holder — a read or member access through it … — is the same compile error
//! cites: 28.2, 28.3.4
//! expect: reject
//! diagnostic: dominat
//! issue: 8708
module Conf;

public class D
  private _n: int;
  public constructor(n: int) _n := n; end constructor;
  public method Ping(): int => _n;
end D;

public class H
  public owned Inner: D;
  public constructor(n: int) Inner := new D(n); end constructor;
  public method Get(): D => Inner;
end H;

public class Program
  public static method Run(): int
    var h := new H(7);
    var v := h.Get();
    h.Dispose();
    return v.Ping();
  end Run;
  public static method Main()
    Console.WriteLine(Program.Run());
  end Main;
end Program;

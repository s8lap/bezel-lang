//! case: s28_2_3604
//! doc: spec
//! section: 28.2
//! title: a member-read holder used only before the variable's disposal is admitted
//! rule: The kill itself stays permitted: what is rejected is the use the kill makes certain to fault,
//!   and a disposal after the borrow's last use changes no verdict.
//! cites: 28.2, 28.3.4
//! expect: run
//! stdout: 7
//! issue: 8708
//! notes: Accepting twin of s28_2_3601: the one edit is the read of `v` moving before the kill.
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
    var n := v.Ping();
    h.Dispose();
    return n;
  end Run;
  public static method Main()
    Console.WriteLine(Program.Run());
  end Main;
end Program;

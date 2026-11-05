//! case: s6_6_2101
//! doc: spec
//! section: 6.6
//! title: a DateTime field += at MaxValue behaves as the explicit x := x +
//!   e
//! rule: For every operator above except `??=`, the statement `x op= e` is
//!   equivalent to `x := x op e` with the target evaluated once:
//! cites: 2.5
//! expect: run
//! output: True
//! issue: 8245
//! notes: Red on base: the pre-fix compiler skipped the DateTime
//!   MinValue..MaxValue range check for a compound assignment whose target
//!   is a field, array element or dictionary slot, while the explicit x :=
//!   x op e raised OverflowException. It passes on the staging tip.
module Conf;

public class H
  public D: DateTime;

  public constructor(d: DateTime)
    D := d;
  end constructor;
end H;

public class Program
  public static method Main()
    var one := TimeSpan.FromTicks(1L);
    var a := new H(DateTime.MaxValue);
    var viaCompound := false;
    try
      a.D += one;
    catch OverflowException =>
      viaCompound := true;
    end;
    var b := new H(DateTime.MaxValue);
    var viaExplicit := false;
    try
      b.D := b.D + one;
    catch OverflowException =>
      viaExplicit := true;
    end;
    Console.WriteLine(viaCompound = viaExplicit);
  end Main;
end Program;

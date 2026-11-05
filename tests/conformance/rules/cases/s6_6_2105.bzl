//! case: s6_6_2105
//! doc: spec
//! section: 6.6
//! title: a DateTime array element -= at MinValue behaves as the explicit x
//!   := x - e
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

public class Program
  public static method Main()
    var one := TimeSpan.FromTicks(1L);
    var a := new Array<DateTime>(1, DateTime.MinValue);
    var viaCompound := false;
    try
      a[0] -= one;
    catch OverflowException =>
      viaCompound := true;
    end;
    var b := new Array<DateTime>(1, DateTime.MinValue);
    var viaExplicit := false;
    try
      b[0] := b[0] - one;
    catch OverflowException =>
      viaExplicit := true;
    end;
    Console.WriteLine(viaCompound = viaExplicit);
  end Main;
end Program;

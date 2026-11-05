//! case: s6_6_2106
//! doc: spec
//! section: 6.6
//! title: an array index in a DateTime compound assignment is evaluated
//!   once on the overflow path
//! rule: For every operator above except `??=`, the statement `x op= e` is
//!   equivalent to `x := x op e` with the target evaluated once:
//! cites: 6.6, 2.5
//! expect: run
//! output: 1
//!   True
//! issue: 8245
//! notes: Red on base: the pre-fix compiler skipped the DateTime
//!   MinValue..MaxValue range check for a compound assignment whose target
//!   is a field, array element or dictionary slot, while the explicit x :=
//!   x op e raised OverflowException. It passes on the staging tip.
module Conf;

public class Program
  public static cnt: int := 0;

  public static method Idx(): int
    cnt := cnt + 1;
    return 0;
  end Idx;

  public static method Main()
    var one := TimeSpan.FromTicks(1L);
    var a := new Array<DateTime>(1, DateTime.MaxValue);
    var viaCompound := false;
    try
      a[Idx()] += one;
    catch OverflowException =>
      viaCompound := true;
    end;
    Console.WriteLine(cnt);
    var b := new Array<DateTime>(1, DateTime.MaxValue);
    var viaExplicit := false;
    try
      b[0] := b[0] + one;
    catch OverflowException =>
      viaExplicit := true;
    end;
    Console.WriteLine(viaCompound = viaExplicit);
  end Main;
end Program;

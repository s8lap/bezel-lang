//! case: s17_8_0005
//! doc: spec
//! section: 17.8
//! title: for-in over a null-forgiving conditional of an owned mint disposes each frame once
//! rule: x! over a named or call-produced owned value is owned and keeps x's own classification; it neither moves nor copies (spec 17.8). A loop over it consumes the iterable exactly once and every enumerator finally runs once, so the totals below are the arithmetic of the loop. Each of 50 iterations sums 1 + 2 and runs the one started frame's finally once; the constructor's Fld frame is never started.
//! cites: 17.8, 29.7, 28.3
//! expect: run
//! output: sum=150 fins=50
module Conf;
public class H
  public static fins: int := 0;
  public owned Fld: IEnumerable<int>?;
  public constructor() Fld := this.GenS(); end constructor;
  public method GenS(): owned IEnumerable<int>
    try yield return 1; yield return 2;
    finally H.fins := H.fins + 1; end;
  end GenS;
  public method Mk(): owned IEnumerable<int>?
    return this.GenS();
  end Mk;
  public property Prop: IEnumerable<int>?
    get => this.Fld;
  end Prop;
end H;

public class Program
  public static method Main(): int
    var sum := 0; var i := 0;
    while i < 50 do
      using var h := new H();
      var c := i > -1;
      for var x in (if c then (if c then h.Mk() else null) else h.Mk())! do sum := sum + x; end;
      i := i + 1;
    end;
    Console.WriteLine("sum=" + sum.ToString() + " fins=" + H.fins.ToString());
    return 0;
  end Main;
end Program;

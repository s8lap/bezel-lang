//! case: s17_8_0007
//! doc: spec
//! section: 17.8
//! title: for-in with break over a null-forgiving conditional runs the abandoned frame finally once
//! rule: x! over a named or call-produced owned value is owned and keeps x's own classification; it neither moves nor copies (spec 17.8). A loop over it consumes the iterable exactly once and every enumerator finally runs once, so the totals below are the arithmetic of the loop. The break after the first element abandons the started frame, whose finally runs once (spec 29.7): 25 even iterations add 1 each.
//! cites: 17.8, 29.7
//! expect: run
//! output: sum=25 fins=25 caught=25
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
    var sum := 0; var i := 0; var caught := 0;
    while i < 50 do
      using var h := new H();
      var c := (i / 2) * 2 = i;
      try
        for var x in (if c then h.Mk() else throw new Exception("x"))! do sum := sum + x; break; end;
      catch
        Exception => do caught := caught + 1; end,
      end try;
      i := i + 1;
    end;
    Console.WriteLine("sum=" + sum.ToString() + " fins=" + H.fins.ToString() + " caught=" + caught.ToString());
    return 0;
  end Main;
end Program;

//! case: s17_8_0008
//! doc: spec
//! section: 17.8
//! title: a var bound to a null-forgiving owned mint and returned through a helper is iterated once
//! rule: x! over a named or call-produced owned value is owned and keeps x's own classification; it neither moves nor copies (spec 17.8). A loop over it consumes the iterable exactly once and every enumerator finally runs once, so the totals below are the arithmetic of the loop. Sink.Make returns the owned var; the caller's var is owned, iterated to the end (sum 3, finally once) per iteration.
//! cites: 17.8, 28.3, 29.7
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
public class Sink
  public owned Held: IEnumerable<int>?;
  public constructor() end constructor;
  public method Take(owned e: IEnumerable<int>?) Held := e; end Take;
  public static method Eat(owned e: IEnumerable<int>?) end Eat;
  public static method Make(h: H): owned IEnumerable<int>?
    var e := h.Mk()!; return e;
  end Make;
end Sink;
public class Program
  public static method Main(): int
    var sum := 0; var i := 0;
    while i < 50 do
      using var h := new H();
      using var s := new Sink();
      var c := i > -1;
      var e := Sink.Make(h)!; for var x in e do sum := sum + x; end;
      i := i + 1;
    end;
    Console.WriteLine("sum=" + sum.ToString() + " fins=" + H.fins.ToString());
    return 0;
  end Main;
end Program;

//! case: s17_8_0013
//! doc: spec
//! section: 17.8
//! title: Reassigning a var bound to a null-forgiving owned mint drops the owned value and is rejected
//! rule: x! of an owned value is owned (spec 17.8), so assigning a new owned value over the var drops the previous one undisposed, rejected exactly as for the bare var (spec 28.3).
//! cites: 17.8, 28.3
//! expect: reject
//! diagnostic: dropped
//! issue: 7678
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
    return null;
  end Make;
end Sink;
public class Program
  public static method Main(): int
    var sum := 0; var i := 0;
    while i < 50 do
      using var h := new H();
      using var s := new Sink();
      var c := i > -1;
      var e := h.Mk()!; for var x in e do sum := sum + x; end; e := h.Mk()!; for var x in e do sum := sum + x; end;
      i := i + 1;
    end;
    Console.WriteLine("sum=" + sum.ToString() + " fins=" + H.fins.ToString());
    return 0;
  end Main;
end Program;

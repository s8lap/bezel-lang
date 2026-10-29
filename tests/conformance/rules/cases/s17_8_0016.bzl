//! case: s17_8_0016
//! doc: spec
//! section: 17.8
//! title: (x)! over an owned mint stays owned and move transfers it to an owned parameter
//! rule: The null-forgiving operator preserves ownership: if `x` has ownership state owned ... `x!` is owned; if `x` is borrowed, `x!` is borrowed with the same source. ... At a marked position — an `owned` argument, a local binding or assignment (K3.3, K3.4) — the transfer is spelled `move x!`, the `!` being transparent to the marker
//! cites: 28.3, 28.8, 28.8.1
//! expect: run
//! output: sum=300 fins=100 ds=0
module Conf;
public class W
  public static ds: int := 0;
  public v: int;
  public constructor(v: int) this.v := v; end constructor;
  destructor W.ds := W.ds + 1; end destructor;
end W;
public class H
  public static fins: int := 0;
  public owned Fld: IEnumerable<int>?;
  private owned _arr: Array<int> := new Array<int>();
  public constructor() end constructor;
  public method GenS(): owned IEnumerable<int>
    try yield return 1; yield return 2;
    finally H.fins := H.fins + 1; end;
  end GenS;
  public method Mk(): owned IEnumerable<int>?
    return this.GenS();
  end Mk;
  public method MkW(): owned W? return new W(3); end MkW;
  public method Backing(): Array<int>? => _arr;
  public property Prop: IEnumerable<int>?
    get => this.Fld;
  end Prop;
end H;
public class Sink
  public owned Held: IEnumerable<int>?;
  public constructor() end constructor;
  public method Take(owned e: IEnumerable<int>?) Held := e; end Take;
  public method SumHeld(): int
    var t := 0;
    for var x in Held! do t := t + x; end;
    return t;
  end SumHeld;
  public static method EatW(owned w: W?) end EatW;
  public static method EatA(owned a: Array<int>?) end EatA;
end Sink;
public class C
  public static method Main(): int
    var sum := 0; var i := 0;
    while i < 100 do
      using var h := new H();
      using var s := new Sink();
      var e := (h.Mk())!; s.Take(move e); sum := sum + s.SumHeld();
      i := i + 1;
    end;
    Console.WriteLine("sum=" + sum.ToString() + " fins=" + H.fins.ToString() + " ds=" + W.ds.ToString());
    return 0;
  end Main;
end C;

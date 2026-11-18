//! case: l4_4_6508
//! doc: stdlib
//! section: 4.4
//! title: a concrete `Array<string>` built inside a generic body from a
//!   string parameter fills every slot
//! rule: Count = size, every slot = fill
//! cites: 4.4, 28.11
//! expect: run
//! output: 1200
//!   heap7
module Conf;
class H
  private _n: int;
  public constructor(n: int)
    _n := n;
  end constructor;
  public method Mint(): string
    return "KM" + _n.ToString();
  end Mint;
end H;
class G
  public static method LocInGen<T>(x: T, t: string): int
    var fa := new Array<string>(2, t);
    fa[0] := "z";
    return fa[1].Length;
  end LocInGen;
end G;
public class C
  public static method Main()
    var h := new H(7);
    var s := "heap" + 7.ToString();
    var tot := 0;
    var i := 0;
    while i < 200 do
      i := i + 1;
      var n := 0;
      var t := h.Mint();
      n := n + G.LocInGen(s, t) + t.Length;
      tot := tot + n;
    end while;
    Console.WriteLine(tot.ToString());
    Console.WriteLine(s);
  end Main;
end C;

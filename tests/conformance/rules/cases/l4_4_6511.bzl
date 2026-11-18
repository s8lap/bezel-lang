//! case: l4_4_6511
//! doc: stdlib
//! section: 4.4
//! title: `new Array<string>(size, fill)` over a produced string fills
//!   every slot
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
public class C
  public static method Main()
    var h := new H(7);
    var s := "heap" + 7.ToString();
    var tot := 0;
    var i := 0;
    while i < 200 do
      i := i + 1;
      var n := 0;
      var a := new Array<string>(3, h.Mint());
      n := n + a.Count + a[2].Length;
      tot := tot + n;
    end while;
    Console.WriteLine(tot.ToString());
    Console.WriteLine(s);
  end Main;
end C;

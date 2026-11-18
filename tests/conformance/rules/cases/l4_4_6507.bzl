//! case: l4_4_6507
//! doc: stdlib
//! section: 4.4
//! title: a concrete `Array<string>` built inside a generic body from a
//!   produced string fills every slot
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
  public static method ConcInGen<T>(x: T, h: H): int
    var fa := new Array<string>(3, h.Mint());
    return fa.Count + fa[2].Length;
  end ConcInGen;
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
      n := n + G.ConcInGen(s, h);
      tot := tot + n;
    end while;
    Console.WriteLine(tot.ToString());
    Console.WriteLine(s);
  end Main;
end C;

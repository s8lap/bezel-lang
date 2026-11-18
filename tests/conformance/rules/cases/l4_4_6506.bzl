//! case: l4_4_6506
//! doc: stdlib
//! section: 4.4
//! title: a generic class filling `Array<T>` at T = string fills every slot
//! rule: Count = size, every slot = fill
//! cites: 4.4, 28.11
//! expect: run
//! output: 600
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
class W<T>
  private _x: T;
  public constructor(x: T)
    _x := x;
  end constructor;
  public method F(): int
    var fa := new Array<T>(3, _x);
    return fa.Count;
  end F;
end W;
public class C
  public static method Main()
    var h := new H(7);
    var s := "heap" + 7.ToString();
    var tot := 0;
    var i := 0;
    while i < 200 do
      i := i + 1;
      var n := 0;
      var w := new W<string>(s);
      n := n + w.F();
      tot := tot + n;
    end while;
    Console.WriteLine(tot.ToString());
    Console.WriteLine(s);
  end Main;
end C;

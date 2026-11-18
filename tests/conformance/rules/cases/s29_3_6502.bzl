//! case: s29_3_6502
//! doc: spec
//! section: 29.3
//! title: `yield break` out of a `for` over a produced string in an iterator
//!   with an owned parameter
//! rule: `yield break;` terminates iteration.
//! cites: 29.3, 29.8, 21.4
//! expect: run
//! stdout: 798
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
class Res
  public _v: int;
  public constructor(v: int)
    _v := v;
  end constructor;
  destructor
    Console.Write("");
  end destructor;
end Res;
class C
  public static method It(owned r: Res, h: H, n: int): IEnumerable<int>
    for var c in h.Mint() do
      if n > 1 then
        yield break;
      end if;
      yield return r._v;
    end for;
  end It;
  public static method Main()
    var tot := 0;
    var h := new H(7);
    for var i in 1..200 do
      for var v in C.It(new Res(2), h, i mod 3) do
        tot := tot + v;
      end for;
    end for;
    Console.WriteLine(tot.ToString());
  end Main;
end C;

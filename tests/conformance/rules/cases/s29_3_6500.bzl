//! case: s29_3_6500
//! doc: spec
//! section: 29.3
//! title: `yield break` out of a `for` over a produced string inside an
//!   iterator ends the iteration
//! rule: `yield break;` terminates iteration.
//! cites: 29.3, 21.4
//! expect: run
//! stdout: 200
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
class C
  public static method It(h: H): IEnumerable<int>
    var tot := 0;
    yield return 0;
    for var c in h.Mint() do
      tot := tot + 1;
      if tot > 1 then
        yield break;
      end if;
      yield return tot;
    end for;
    yield return tot;
  end It;
  public static method Main()
    var tot := 0;
    var h := new H(7);
    for var i in 0..<200 do
      for var v in C.It(h) do
        tot := tot + v;
      end for;
    end for;
    Console.WriteLine(tot.ToString());
  end Main;
end C;

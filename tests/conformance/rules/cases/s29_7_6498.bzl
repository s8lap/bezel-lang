//! case: s29_7_6498
//! doc: spec
//! section: 29.7
//! title: an iterator abandoned by the consumer inside a `for` over a
//!   produced string is disposed cleanly
//! rule: When a consumer stops iterating early — `break`, exception, or
//!   `using var` ending before exhaustion — the iterator is disposed.
//! cites: 29.7, 29.3, 21.4
//! expect: run
//! stdout: 400
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
  public static method It(h: H): IEnumerable<int>
    using var r := new Res(1);
    for var c in h.Mint() do
      yield return 1;
    end for;
    yield break;
  end It;
  public static method Main()
    var tot := 0;
    var h := new H(7);
    for var i in 0..<200 do
      for var v in C.It(h) do
        tot := tot + v;
        if tot mod 2 = 0 then
          break;
        end if;
      end for;
    end for;
    Console.WriteLine(tot.ToString());
  end Main;
end C;

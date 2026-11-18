//! case: s21_4_6499
//! doc: spec
//! section: 21.4
//! title: `for` over a produced string in an async method, awaiting and
//!   returning from inside the loop
//! rule: `for` iterates any iterable source.
//! cites: 21.4, 30.1
//! expect: run
//! stdout: 225
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
  public static async method Run(h: H, n: int): Task<int>
    var tot := 0;
    for var c in h.Mint() do
      await Task.Delay(0);
      tot := tot + 1;
      if tot > n then
        return tot;
      end if;
    end for;
    return tot;
  end Run;
  public static async method Main(): Task<int>
    var tot := 0;
    var i := 0;
    var h := new H(7);
    while i < 100 do
      i := i + 1;
      tot := tot + await C.Run(h, i mod 4);
    end while;
    Console.WriteLine(tot.ToString());
    return 0;
  end Main;
end C;

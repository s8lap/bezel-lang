//! case: s28_3_10_7554
//! doc: spec
//! section: 28.3.10
//! title: a borrow alias stored into an owned field is rejected
//! rule: is a borrow of `x` and discharges nothing
//! cites: 28.3.10
//! expect: reject
//! diagnostic: borrow cannot be stored into an `owned` field
module Conf;
class C
  private owned saved: Array<int>?;
  public method Go(cond: bool): int
    var ys: Array<int> := [1, 2];
    var b := ys;
    if cond then this.saved := b; end;
    return ys.Count;
  end Go;
  public static method Main(): int
    var n := 0;
    for var i in 1..4 do
      using var c := new C();
      n := n + c.Go(false);
    end;
    Console.WriteLine(n);
    return 0;
  end Main;
end C;

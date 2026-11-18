//! case: s28_3_1_7555
//! doc: spec
//! section: 28.3.1
//! title: an owned and a borrowed arm joined into one local is rejected
//! rule: owned ⊓ borrowed = **rejected**
//! cites: 28.3.1
//! expect: reject
//! diagnostic: branch ownership conflict
module Conf;
class C
  private owned saved: Array<int>?;
  private owned keep: Array<int>?;
  public method Go(cond: bool): int
    this.keep := new Array<int>();
    var ys := new Array<int>(); ys.Add(5);
    if cond then ys := this.keep!; end;
    this.saved := ys;
    return 1;
  end Go;
  public static method Main(): int
    var n := 0;
    for var i in 1..4 do
      using var c := new C();
      n := n + c.Go(i mod 2 = 0);
    end;
    Console.WriteLine(n);
    return 0;
  end Main;
end C;

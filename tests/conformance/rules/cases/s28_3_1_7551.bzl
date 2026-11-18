//! case: s28_3_1_7551
//! doc: spec
//! section: 28.3.1
//! title: a lambda-local read in WriteLine after a conditional owned-field store is rejected
//! rule: = **maybe-consumed** — unreadable after the join
//! cites: 28.3.1
//! expect: reject
//! diagnostic: maybe-consumed
module Conf;
class C
  private owned saved: Array<int>?;
  public method Go(cond: bool): int
    var f := lambda(): int => do
      var ys := new Array<int>(); ys.Add(5);
      if cond then this.saved := ys; end;
      Console.WriteLine(ys.Count);
      return 1;
    end;
    return f();
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

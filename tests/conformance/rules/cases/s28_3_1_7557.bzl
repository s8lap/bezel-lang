//! case: s28_3_1_7557
//! doc: spec
//! section: 28.3.1
//! title: a loop-local array stored into an owned field only on one iteration is disposed per path
//! rule: scope-end disposal per-path
//! cites: 28.3.1
//! expect: run
//! stdout: 12
module Conf;
class C
  private owned saved: Array<int>?;
  private owned keep: Array<int>?;
  public method Go(cond: bool): int
    this.keep := new Array<int>();
    var f := lambda(): int => do
      var k := 0;
      for var j in 1..3 do
        var ys := new Array<int>(); ys.Add(j);
        if cond and j = 2 then this.saved := ys; end;
        k := k + 1;
      end;
      return k;
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

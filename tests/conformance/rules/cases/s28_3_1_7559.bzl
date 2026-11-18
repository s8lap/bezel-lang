//! case: s28_3_1_7559
//! doc: spec
//! section: 28.3.1
//! title: a local moved into an owned field then reassigned is stored again conditionally
//! rule: scope-end disposal per-path
//! cites: 28.3.1
//! expect: run
//! stdout: 4
module Conf;
class C
  private owned saved: Array<int>?;
  private owned keep: Array<int>?;
  public method Go(cond: bool): int
    this.keep := new Array<int>();
    var f := lambda(): int => do
      var ys := new Array<int>(); ys.Add(5);
      this.keep := move ys;
      ys := new Array<int>(); ys.Add(6);
      if cond then this.saved := ys; end;
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

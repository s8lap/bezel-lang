//! case: s28_3_1_7556
//! doc: spec
//! section: 28.3.1
//! title: an if/else storing an owned local into either of two owned fields is admitted
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
      var ys := new Array<int>(); ys.Add(1);
      if cond then this.saved := ys; else this.keep := ys; end;
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

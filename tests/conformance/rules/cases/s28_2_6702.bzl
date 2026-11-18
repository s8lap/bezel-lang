//! case: s28_2_6702
//! doc: spec
//! section: 28.2
//! title: an alias re-bound to the variable's new value takes a fresh borrow after the kill
//! rule: while a holder re-bound to the variable's new value (`b := a;` after `a := new …;`) takes a fresh borrow
//! cites: 28.3
//! expect: run
//! stdout: 3
module Conf;

public class Res
  private _n: int;
  public constructor(n: int)
    _n := n;
  end constructor;
  public method Ping(): int => _n;
end Res;

public class Program
  public static method Run(): int
    var d := new Res(1);
    var b := d;
    var r := b.Ping();
    d.Dispose();
    d := new Res(2);
    b := d;
    return r + b.Ping();
  end Run;
  public static method Main()
    Console.WriteLine(Program.Run());
  end Main;
end Program;

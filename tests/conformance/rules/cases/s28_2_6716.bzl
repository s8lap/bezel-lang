//! case: s28_2_6716
//! doc: spec
//! section: 28.2
//! title: passing a closure holder of a disposed source is an error
//! rule: a use of the holder — a read or member access through it, passing it, re-borrowing it, invoking the closure, enumerating or disposing the frame — is the same compile error
//! cites: 28.3
//! expect: reject
//! diagnostic: dominat
module Conf;

public class Res
  private _n: int;
  public constructor(n: int)
    _n := n;
  end constructor;
  public method Ping(): int => _n;
end Res;

public class Program
  public static method Apply(f: () -> int): int => f();
  public static method Run(): int
    var d := new Res(1);
    var b := d;
    var g := lambda() => b.Ping();
    d.Dispose();
    return Program.Apply(g);
  end Run;
  public static method Main()
    Console.WriteLine(Program.Run());
  end Main;
end Program;

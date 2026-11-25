//! case: s28_14_9986
//! doc: spec
//! section: 28.14
//! title: a lambda over a `scoped` formal may be invoked in place or passed to a call-bounded parameter
//! rule: it may be immediately invoked
//! cites: 28.14, 28.3
//! expect: run
//! output: 600
//! notes: The admitting twin of the #9310 rejections: every spelling invokes the callback within the call (6
//!   hits per On: immediate invoke, `??`, `if`, `case`, a call-bounded argument, a call-bounded local).
//!   100 calls: total = 600.
module Conf;
public class Counter2
  private _n: int := 0;
  public method Hit(t: int)
    _n := _n + 1;
  end Hit;
  public property N: int => _n;
  public constructor() end constructor;
end Counter2;
public class Hub
  private static method Run(f: (int) -> ())
    f(1);
  end Run;
  public method On(scoped handler: (int) -> (), flag: bool)
    (lambda(t: int) => handler(t))(1);
    var g: ((int) -> ())? := null;
    (g ?? lambda(t: int) => handler(t))(1);
    (if flag then lambda(t: int) => handler(t) else lambda(t: int) => Console.Write(""))(1);
    var k := 0;
    (case k of 0 => lambda(t: int) => handler(t), _ => lambda(t: int) => Console.Write(""))(1);
    Run(lambda(t: int) => handler(t));
    var f := lambda(t: int) => handler(t);
    Run(f);
  end On;
end Hub;
public class Program
  public static method Main(): int
    var total := 0;
    for var i in 0 ..< 100 do
      do
        using var c := new Counter2();
        using var hub := new Hub();
        hub.On(lambda(t) => c.Hit(t), true);
        total := total + c.N;
      end;
    end for;
    Console.WriteLine($"{total}");
    return 0;
  end Main;
end Program;

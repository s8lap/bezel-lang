//! case: s28_14_9986
//! doc: spec
//! section: 28.14
//! title: a lambda over a `scoped` formal invoked through nested closures in the callee is rejected
//! rule: Callee-side uses inside a nested closure are escaping uses (*liftable*): invoking a scoped
//!   parameter through a locally-invoked closure is rejected in v1. The bound covers every callee-side use
//!   the closure body makes of the parameter … whether or not the closure escapes the callee
//! cites: 28.14, 28.3
//! expect: reject
//! diagnostic: scoped
//! notes: Was `run` (600), quoting §28.3's general "it may be immediately invoked"; §28.14's bound 6
//!   narrows that admission for a closure over a `scoped` parameter of the enclosing callee, which v0.361
//!   (#9311) states. Every spelling here invokes `handler` through a nested closure.
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

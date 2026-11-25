//! case: s28_14_9209
//! doc: spec
//! section: 28.14
//! title: an expression body may invoke call-bounded `q` and pass it to a plain parameter
//! rule: it may be passed to a function-typed parameter, where the callee's use is bounded by its signature
//! cites: 28.14
//! expect: run
//! output: 10
//! notes: Pins #9142 (fix 727ee98df): the expression-bodied twin of the block-bodied call-bounded sink check.
//!   Admitting twin of 9204: `Plain(q) + q()` invokes `q` twice (5 + 5 = 10). Green on both arms.
module Conf;
public class Sink
  public constructor() end constructor;
  public method Take(scoped cb: () -> int): int
    return 0;
  end Take;
  public method TakeG<T>(scoped cb: () -> T): int
    return 0;
  end TakeG;
  public method Plain(cb: () -> int): int
    return cb();
  end Plain;
end Sink;
public class Fl
  private _l: Array<() -> int> := [];
  public constructor() end constructor;
  public method M(q: () -> int): int => (new Sink()).Plain(q) + q();
end Fl;
public class Program
  public static method Main(): int
    var fl := new Fl();
    Console.WriteLine($"{fl.M(lambda() => 5)}");
    return 0;
  end Main;
end Program;

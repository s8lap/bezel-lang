//! case: s28_14_9210
//! doc: spec
//! section: 28.14
//! title: an expression body may capture call-bounded `q` in a closure passed to a plain parameter
//! rule: it may be captured by another closure, whose set then includes its own
//! cites: 28.14, 28.3
//! expect: run
//! output: 6
//! notes: Pins #9142 (fix 727ee98df): the expression-bodied twin of the block-bodied call-bounded sink check.
//!   Admitting twin of 9205: `Select(lambda(x) => q())` captures `q` in a closure that does not outlive
//!   the call: 2 + 2 + 2 = 6. Green on both arms.
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
  public method M(q: () -> int): int => (1..3).Select(lambda(x) => q()).Sum();
end Fl;
public class Program
  public static method Main(): int
    var fl := new Fl();
    Console.WriteLine($"{fl.M(lambda() => 2)}");
    return 0;
  end Main;
end Program;

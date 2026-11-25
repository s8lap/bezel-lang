//! case: s28_14_9204
//! doc: spec
//! section: 28.14
//! title: an expression body adding call-bounded `q` to a field collection is rejected
//! rule: A function-typed parameter *without* `scoped` is **call-bounded**: the callee may invoke the
//!   callback during the call but may not retain it past the call
//! cites: 28.14
//! expect: reject
//! diagnostic: call-bounded
//! notes: Pins #9142 (fix 727ee98df): the expression-bodied twin of the block-bodied call-bounded sink check.
//!   `this._l.Add(q)` stores the callback in a collection reachable beyond the call. Twin 9209 invokes
//!   `q` instead.
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
  public method M(q: () -> int) => this._l.Add(q);
end Fl;
public class Program
  public static method Main(): int
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;

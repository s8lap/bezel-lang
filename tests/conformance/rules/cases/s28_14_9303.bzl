//! case: s28_14_9303
//! doc: spec
//! section: 28.14
//! title: a lazy query that carries a call-bounded callback and is captured by a returned closure is rejected
//! rule: is an escape of the callback itself, rejected by these same rules applied transitively
//! cites: 28.14, 28.3, 29.11
//! expect: reject
//! diagnostic: call-bounded
//! notes: Harvested from the #9174 review (p_lazy_query_capture). The lazy `Select` mints a query frame that
//!   carries the call-bounded callback `lambda(x) => x + q()` (it calls `q`); the returned closure captures
//!   that frame, so the frame escapes the callee and with it the callback. s28_14_9305's twin builds the same
//!   query without touching `q`, which is legal to capture.
module Conf;
public class Fl
  public constructor() end constructor;
  public method Get(q: () -> int): () -> int
    var s := (1..3).Select(lambda(x: int) => x + q());
    return lambda() => s.Sum();
  end Get;
end Fl;
public class Program
  public static method Main(): int
    using var fl := new Fl();
    var k := 5;
    var f := fl.Get(lambda() => k);
    Console.WriteLine(f());
    return 0;
  end Main;
end Program;

//! case: s28_14_9301
//! doc: spec
//! section: 28.14
//! title: a call-bounded callback bound to a local and captured by a RETURNED closure is rejected
//! rule: returned, captured by a closure that itself escapes the callee (judged by these same rules, applied transitively)
//! cites: 28.14, 28.3
//! expect: reject
//! diagnostic: call-bounded
//! notes: Harvested from the #9174 review (a_capture_via_local). `q` is a plain (call-bounded) function-typed
//!   parameter. Binding it to the local `g` is allowed ("bound to locals"), but `return lambda() => g()`
//!   returns a closure capturing it, so the callback escapes the callee. The only ground is the returned
//!   closure; s28_14_9304 is the accepting twin that invokes `q` and captures the resulting value instead.
module Conf;
public class Fl
  public constructor() end constructor;
  public method Get(q: () -> int): () -> int
    var g := q;
    return lambda() => g();
  end Get;
end Fl;
public class Program
  public static method Main(): int
    using var fl := new Fl();
    var f := fl.Get(lambda() => 5);
    Console.WriteLine(f());
    return 0;
  end Main;
end Program;

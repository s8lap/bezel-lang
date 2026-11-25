//! case: s28_14_9304
//! doc: spec
//! section: 28.14
//! title: a call-bounded callback invoked in the callee, its VALUE captured by a returned closure, is admitted
//! rule: Within the callee the callback may be invoked, bound to locals, and passed on as an argument to another call-bounded parameter
//! cites: 28.14, 28.3
//! expect: run
//! output: 422
//! notes: Harvested from the #9174 review (d_shared_cell), the accepting twin of s28_14_9301. `Get` invokes
//!   `q` twice, keeping only the results, and returns a closure over those ints. The argument closure
//!   captures and mutates the caller's `n`. Per iteration: n = 0; `f := Get(..)` runs q twice: n = 1 then 2,
//!   so a = 1, b = 2 and f() = 3; then n := n + 100 = 102, so t += f() + n = 3 + 102 = 105. `g := Get(..)`:
//!   n = 103, a = 103, then n = 104, b = 104, g() = 207; t += g() + f() + f() + n = 207 + 3 + 3 + 104 = 317.
//!   Total 105 + 317 = 422.
module Conf;
public class Fl
  public constructor() end constructor;
  public method Get(q: () -> int): () -> int
    var a := q();
    var b := q();
    return lambda() => a + b;
  end Get;
end Fl;
public class Program
  public static method Main(): int
    var t := 0;
    using var fl := new Fl();
    var n := 0;
    var f := fl.Get(lambda() => do n := n + 1; return n; end);
    n := n + 100;
    t := t + f() + n;
    var g := fl.Get(lambda() => do n := n + 1; return n; end);
    t := t + g() + f() + f() + n;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;

//! case: s27_5_8830
//! doc: spec
//! section: 27.5
//! title: through a function-typed auto-property the callee reassigns, `P(this)` still reads its capture
//! rule: Captured variables are heap-allocated in a closure object, a `ref` or `out` parameter excepted (below).
//! cites: 28.9, 28.3, 28.3.11
//! expect: run
//! output: 399
//! notes: Pins #9124 (fixes bb855dd97 / 82fe40b0f, review r43-rev-m60-9123, probe b1_prop_getset).
//!  `P(this)` calls the closure held by a property; the body reassigns `P` via `x.Drop()` and then reads `k`. Sum 399.
//!  This is the #9124 property spelling (the non-null-conditional call).
//!  On the parent compiler the plain run diverges (a read of the freed closure environment) and `bezelc --sanitize=address`
//!  reports a heap-use-after-free READ in the lambda body.
module Conf;
public class Hx
  public property P: (Hx) -> int := lambda(x: Hx) => 0;
  public constructor() end constructor;
  public method Fill(k: int) P := lambda(x: Hx) => do x.Drop(); return k + 1; end; end Fill;
  public method Drop() P := lambda(x: Hx) => 0; end Drop;
  public method Run(): int
    return P(this);
  end Run;
end Hx;
public class Program
  public static method Main(): int
    var t := 0;
    for var i in 0 ..< 200 do
      var h := new Hx(); h.Fill(i mod 3); t := t + h.Run();
    end for;
    Console.WriteLine($"{t}");
    return 0;
  end Main;
end Program;

//! case: s27_5_8831
//! doc: spec
//! section: 27.5
//! title: through a nullable function-typed property cleared by the callee, `P?(this)` still reads its capture
//! rule: Captured variables are heap-allocated in a closure object, a `ref` or `out` parameter excepted (below).
//! cites: 28.9, 28.3, 28.3.11
//! expect: run
//! output: 399
//! notes: Pins #9123 (fixes bb855dd97 / 82fe40b0f, review r43-rev-m60-9123, probe b4_prop_nullable_q).
//!  `P?(this) ?? 7`: the property is cleared (set to null) by the body mid-call, which then reads `k`. Sum 399.
//!  On the parent compiler the plain run diverges (a read of the freed closure environment) and `bezelc --sanitize=address`
//!  reports a heap-use-after-free READ in the lambda body.
module Conf;
public class Hx
  public property P: ((Hx) -> int)?;
  public constructor() end constructor;
  public method Fill(k: int) P := lambda(x: Hx) => do x.Drop(); return k + 1; end; end Fill;
  public method Drop() P := null; end Drop;
  public method Run(): int
    return P?(this) ?? 7;
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

//! case: s27_5_8827
//! doc: spec
//! section: 27.5
//! title: `_f?(this, this.DropI())` with the field cleared by an argument still reads its capture
//! rule: Captured variables are heap-allocated in a closure object, a `ref` or `out` parameter excepted (below).
//! cites: 28.9, 28.3, 28.3.11
//! expect: run
//! output: 399
//! notes: Pins #9123 (fixes bb855dd97 / 82fe40b0f, review r43-rev-m60-9123, probe a3_argdrop).
//!  The argument `this.DropI()` is evaluated after the callee is read and CLEARS `_f`; the invoked closure still reads its
//!  captured `k`: k + 1 + 0 per iteration, sum 399.
//!  On the parent compiler the plain run diverges (a read of the freed closure environment) and `bezelc --sanitize=address`
//!  reports a heap-use-after-free READ in the lambda body.
module Conf;
public class Hx
  private _f: ((Hx, int) -> int)? := null;
  public constructor() end constructor;
  public method Fill(k: int) _f := lambda(x: Hx, d: int) => k + 1 + d; end Fill;
  public method DropI(): int _f := null; return 0; end DropI;
  public method Run(): int
    return _f?(this, this.DropI()) ?? 7;
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

//! case: s27_5_8826
//! doc: spec
//! section: 27.5
//! title: `_f?(this)` on a field the callee reassigns mid-call still reads its capture
//! rule: Captured variables are heap-allocated in a closure object, a `ref` or `out` parameter excepted (below).
//! cites: 28.9, 28.3, 28.3.11
//! expect: run
//! output: 399
//! notes: Pins #9123 (fixes bb855dd97 / 82fe40b0f, review r43-rev-m60-9123, probe a2_reassign).
//!  `_f?(this)` (null-conditional spelling) invokes a closure held in a field; the body calls `x.Drop()`, which REASSIGNS `_f`,
//!  and then reads its captured `k`. Per i: k = i mod 3, so the result is k + 1 and the sum over 200 iterations is 399.
//!  On the parent compiler the plain run diverges (a read of the freed closure environment) and `bezelc --sanitize=address`
//!  reports a heap-use-after-free READ in the lambda body.
module Conf;
public class Hx
  private _f: ((Hx) -> int)? := null;
  public constructor() end constructor;
  public method Fill(k: int) _f := lambda(x: Hx) => do x.Drop(); return k + 1; end; end Fill;
  public method Drop() _f := lambda(x: Hx) => 100; end Drop;
  public method Run(): int
    return _f?(this) ?? 7;
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

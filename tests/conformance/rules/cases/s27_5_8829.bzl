//! case: s27_5_8829
//! doc: spec
//! section: 27.5
//! title: a statement-form `_f?(this);` on a void function-typed field keeps its capture alive
//! rule: Captured variables are heap-allocated in a closure object, a `ref` or `out` parameter excepted (below).
//! cites: 28.9, 28.3, 28.3.11
//! expect: run
//! output: 399
//! notes: Pins #9123 (fixes bb855dd97 / 82fe40b0f, review r43-rev-m60-9123, probe a7_void).
//!  A statement-form `_f?(this);` of a `(Hx) -> ()` field; the body clears the field then stores `k + 1` into `x.v`.
//!  Run returns v = k + 1 per iteration, sum 399.
//!  On the parent compiler the plain run diverges (a read of the freed closure environment) and `bezelc --sanitize=address`
//!  reports a heap-use-after-free READ in the lambda body.
module Conf;
public class Hx
  private _f: ((Hx) -> ())? := null;
  public v: int := 0;
  public constructor() end constructor;
  public method Fill(k: int) _f := lambda(x: Hx) => do x.Drop(); x.v := k + 1; end; end Fill;
  public method Drop() _f := null; end Drop;
  public method Run(): int
    _f?(this);
    return v;
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

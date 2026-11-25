//! case: s27_5_8833
//! doc: spec
//! section: 27.5
//! title: in an async method, `_f?(this)` on a function-typed field still reads its capture after a clear
//! rule: Captured variables are heap-allocated in a closure object, a `ref` or `out` parameter excepted (below).
//! cites: 28.9, 28.3, 28.3.11
//! expect: run
//! output: 399
//! notes: Pins #9123 (fixes bb855dd97 / 82fe40b0f, review r43-rev-m60-9123, probe e1_async_q).
//!  `await Task.Delay(1)` then `_f?(this) ?? 7` inside an async method; the body clears the field and reads `k`. Sum 399.
//!  On the parent compiler the plain run diverges (a read of the freed closure environment) and `bezelc --sanitize=address`
//!  reports a heap-use-after-free READ in the lambda body.
module Conf;
public class Hx
  private _f: ((Hx) -> int)? := null;
  public constructor() end constructor;
  public method Fill(k: int) _f := lambda(x: Hx) => do x.Drop(); return k + 1; end; end Fill;
  public method Drop() _f := null; end Drop;
  public async method Run(): Task<int>
    await Task.Delay(1);
    return _f?(this) ?? 7;
  end Run;
end Hx;
public class Program
  public static async method Main(): Task<int>
    var t := 0;
    for var i in 0 ..< 200 do
      var h := new Hx(); h.Fill(i mod 3); t := t + await h.Run();
    end for;
    Console.WriteLine($"{t}");
    return 0;
  end Main;
end Program;

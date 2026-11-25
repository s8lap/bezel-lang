//! case: s22_3_9913
//! doc: spec
//! section: 22.3
//! title: A ref nullable function field argument takes a value over null and over another holder's value
//! rule: A `ref` argument naming a mutable local or writable field is a true alias: callee writes are
//!   visible through the caller's location immediately
//! cites: 22.3, 28.9, 27.2
//! expect: run
//! stdout: 21100
//! notes: Harvested from the #9249 review (r43-rev-c79-9249-ff). The callee stores the source's function
//!   value into the `ref` field target; both holders then hold the same runtime-managed closure
//!   (28.9). The pre-fix compiler over-released the closure (aborting the run). The generic,
//!   extension, cross-module, virtual, looping, forwarding and two-ref callee shapes of #9249 are not
//!   covered here. a.F starts null; Copy gives it b.F (i), then keep.F (3); a self-copy leaves keep.F
//!   at 3: i + 3 + 3 per iteration, 19900 + 1200 = 21100.
module Conf;

public class Hold
  public F: (() -> int)?;
  public constructor() end constructor;
end Hold;

public class Program
  public static method Copy(ref x: (() -> int)?, src: Hold) x := src.F; end Copy;

  public static method Main(): int
    var sum := 0;
    var keep := new Hold();
    keep.F := lambda() => 3;
    for var i in 0 ..< 200 do
      var a := new Hold();
      var b := new Hold();
      b.F := lambda() => i;
      Copy(ref a.F, b);
      sum := sum + a.F!();
      Copy(ref a.F, keep);
      Copy(ref keep.F, keep);
      sum := sum + a.F!() + keep.F!();
    end for;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;

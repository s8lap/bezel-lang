//! case: s22_3_9910
//! doc: spec
//! section: 22.3
//! title: A ref bare-field argument in a constructor takes the source's function value
//! rule: A `ref` argument naming a mutable local or writable field is a true alias: callee writes are
//!   visible through the caller's location immediately
//! cites: 22.3, 28.9
//! expect: run
//! stdout: 800
//! notes: Harvested from the #9249 review (r43-rev-c79-9249-ff). The callee stores the source's function
//!   value into the `ref` field target; both holders then hold the same runtime-managed closure
//!   (28.9). The pre-fix compiler over-released the closure (aborting the run). The generic,
//!   extension, cross-module, virtual, looping, forwarding and two-ref callee shapes of #9249 are not
//!   covered here. Copy overwrites Box.F with b.F (returns 2), so x.F() + b.F() = 4 per iteration,
//!   800.
module Conf;

public class Hold
  public F: () -> int;
  public constructor(n: int) F := lambda() => n; end constructor;
end Hold;

public class Box
  public F: () -> int;
  public constructor(b: Hold) F := lambda() => 7; Program.Copy(ref F, b); end constructor;
end Box;

public class Program
  public static method Copy(ref x: () -> int, src: Hold) x := src.F; end Copy;

  public static method Main(): int
    var sum := 0;
    for var i in 0 ..< 200 do
      var b := new Hold(2);
      var x := new Box(b);
      sum := sum + x.F() + b.F();
    end for;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;

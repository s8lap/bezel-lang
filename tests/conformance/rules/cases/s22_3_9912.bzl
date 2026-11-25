//! case: s22_3_9912
//! doc: spec
//! section: 22.3
//! title: A ref field argument in an async caller copies the source's function value
//! rule: A `ref` argument naming a mutable local or writable field is a true alias: callee writes are
//!   visible through the caller's location immediately
//! cites: 22.3, 28.9
//! expect: run
//! stdout: 800
//! notes: Harvested from the #9249 review (r43-rev-c79-9249-ff). The callee stores the source's function
//!   value into the `ref` field target; both holders then hold the same runtime-managed closure
//!   (28.9). The pre-fix compiler over-released the closure (aborting the run). The generic,
//!   extension, cross-module, virtual, looping, forwarding and two-ref callee shapes of #9249 are not
//!   covered here. a.F becomes b.F (2) before the await; 2 + 2 + 0 = 4 per call, 800.
module Conf;
import System.Threading.Tasks;

public class Hold
  public F: () -> int;
  public constructor(n: int) F := lambda() => n; end constructor;
end Hold;

public class Program
  public static method Copy(ref x: () -> int, src: Hold) x := src.F; end Copy;

  public static async method Run(): Task<int>
    var a := new Hold(1);
    var b := new Hold(2);
    Copy(ref a.F, b);
    var z := await Task.FromResult(0);
    return a.F() + b.F() + z;
  end Run;

  public static method Main(): int
    var sum := 0;
    for var i in 0 ..< 200 do
      sum := sum + Run().Result;
    end for;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;

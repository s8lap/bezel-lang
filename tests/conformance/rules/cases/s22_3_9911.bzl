//! case: s22_3_9911
//! doc: spec
//! section: 22.3
//! title: A ref field argument through an inherited field copies the source's function value
//! rule: A `ref` argument naming a mutable local or writable field is a true alias: callee writes are
//!   visible through the caller's location immediately
//! cites: 22.3, 28.9, 14.1
//! expect: run
//! stdout: 800
//! notes: Harvested from the #9249 review (r43-rev-c79-9249-ff). The callee stores the source's function
//!   value into the `ref` field target; both holders then hold the same runtime-managed closure
//!   (28.9). The pre-fix compiler over-released the closure (aborting the run). The generic,
//!   extension, cross-module, virtual, looping, forwarding and two-ref callee shapes of #9249 are not
//!   covered here. After Copy a.F is b.F (2): 2 + 2 = 4 per call, 800. The caller is generic, the
//!   callee is not.
module Conf;

public class Base
  public F: () -> int;
  public constructor(n: int) F := lambda() => n; end constructor;
end Base;

public class Derived(n: int): Base(n)
end Derived;

public class Program
  public static method Copy(ref x: () -> int, src: Derived) x := src.F; end Copy;

  public static method Run<T>(t: T): int
    var a := new Derived(1);
    var b := new Derived(2);
    Copy(ref a.F, b);
    return a.F() + b.F();
  end Run;

  public static method Main(): int
    var sum := 0;
    for var i in 0 ..< 200 do
      sum := sum + Run<int>(0);
    end for;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;

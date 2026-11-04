//! case: s33_3_0008
//! doc: spec
//! section: 33.3
//! title: Environment.Exit inside the exit pass does not re-run disposals the pass already performed
//! rule: **Inside the exit pass** — from a destructor the pass runs, or from a lazy
//!   static initializer a first touch there triggers (§22.13) — the call neither
//!   re-initiates the sequence nor is a no-op: it ends the pass where it stands …
//!   nothing already disposed is disposed again, and nothing not yet disposed is
//!   disposed twice.
//! cites: 33.3, 22.13
//! expect: run
//! output: main
//!   closed 2
//! notes: reverse declaration order disposes Second ("closed 2") before First, whose
//!   destructor calls Exit; a re-initiated sequence would dispose Second again, and
//!   a no-op Exit would let the pass go on to Zero and print "closed 0".
module Conf;

public class Res
  public property N: int;

  public constructor(n: int)
    N := n;
  end constructor;

  destructor
    Console.WriteLine($"closed {N}");
  end destructor;
end Res;

public class Late
  public constructor()
  end constructor;

  destructor
    Environment.Exit(0);
  end destructor;
end Late;

public class App
  public static owned readonly Zero: Res := new Res(0);
  public static owned readonly First: Late := new Late();
  public static owned readonly Second: Res := new Res(2);

  public static method Main()
    Console.WriteLine("main");
  end Main;
end App;

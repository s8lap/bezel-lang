//! case: s33_3_0002
//! doc: spec
//! section: 33.3
//! title: Environment.Exit inside the exit pass ends the pass and leaves unreached statics undisposed
//! rule: **Inside the exit pass** — from a destructor the pass runs, or from a
//!   lazy static initializer a first touch there triggers (§22.13) — the call
//!   neither re-initiates the sequence nor is a no-op: it ends the pass where
//!   it stands, and the process exits at once with its `code` ... every static
//!   `owned` field and owning-inline-array slot the pass had not yet reached
//!   is reclaimed by the process exit, never disposed
//! cites: 33.3, 22.13, 28.3
//! expect: run
//! output: 1
//! issue: 5901
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
    Environment.Exit(3);
  end destructor;
end Late;

public class App
  public static owned readonly First: Res := new Res(1);
  public static owned readonly Second: Late := new Late();

  public static method Main()
    Console.WriteLine(First.N);
  end Main;
end App;

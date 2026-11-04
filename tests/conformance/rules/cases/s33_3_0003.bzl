//! case: s33_3_0003
//! doc: spec
//! section: 33.3
//! title: Environment.Exit runs the static owned disposal pass but not the live frame's using var
//! rule: **`Environment.Exit(code)`:** … it initiates the same shutdown sequence (no
//!   further continuations anywhere, static `owned` disposal on the dedicated
//!   shutdown thread, then process exit with `code`). It never returns.
//! cites: 33.3, 22.13
//! expect: run
//! output: held
//!   closed held
//! notes: `Held` is initialized at App's first use and disposed by the exit pass;
//!   the `using var` of Main's live frame is reclaimed, not disposed (s33_3_0001).
module Conf;

public class Res
  public property Name: string;

  public constructor(name: string)
    Name := name;
  end constructor;

  destructor
    Console.WriteLine($"closed {Name}");
  end destructor;
end Res;

public class App
  public static owned readonly Held: Res := new Res("held");

  public static method Main()
    using var local := new Res("local");
    Console.WriteLine(Held.Name);
    Environment.Exit(0);
  end Main;
end App;

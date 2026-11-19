//! case: s28_9_7725
//! doc: spec
//! section: 28.9
//! title: a function-typed block getter read into a nullable local yields the current closure
//! rule: a getter never transfers a reclamation obligation to its reader
//! cites: 23.3
//! expect: run
//! output: 5
//!   6
module Conf;
import System;
public class H
  private f: () -> int;
  public constructor() f := lambda(): int => 5; end constructor;
  public method Reset() f := lambda(): int => 6; end Reset;
  public property NP: (() -> int)?
    get => do return f; end;
  end NP;
end H;
public class Program
  public static method Main(): int
    using var h := new H();
    var x := h.NP;
    if x <> null then Console.WriteLine(x!()); end;
    h.Reset();
    var y := h.NP;
    if y <> null then Console.WriteLine(y!()); end;
    return 0;
  end Main;
end Program;

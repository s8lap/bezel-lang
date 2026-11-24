//! case: s28_3_8227
//! doc: spec
//! section: 28.3
//! title: a field initializer lambda capturing an int parameter and passed at an Object parameter
//!   is admitted
//! rule: a captured value that carries no borrow — a value type that holds none, a null-source
//!   value, a `string` — contributes nothing
//! cites: 28.3
//! expect: run
//! stdout: 1
//! notes: Over-rejection guard for #8963: the lambda passed at `Object` captures `n`, an `int`
//!   parameter that carries no borrow, so its source set is empty. `Keep` returns 1.
module Conf;
import System;
public class Reg2
  public constructor() end constructor;
  public method Keep(o: Object): int return 1; end Keep;
  public method Take(f: () -> int): int return f(); end Take;
end Reg2;
public class Fl
  public H: (Reg2, int) -> int := lambda(rg: Reg2, n: int): int => rg.Keep(lambda(): int => n);
  public constructor() end constructor;
end Fl;
public class Program
  public static method Main(): int
    var reg := new Reg2();
    var fl := new Fl();
    Console.WriteLine(fl.H(reg, 7));
    return 0;
  end Main;
end Program;

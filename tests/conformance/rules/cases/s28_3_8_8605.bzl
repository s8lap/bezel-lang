//! case: s28_3_8_8605
//! doc: spec
//! section: 28.3.8
//! title: an out call over a local that held a call-bounded function value leaves it capture-free
//! rule: on the call's normal return an `out` location carries the empty set
//! cites: 28.3.8, 22.3
//! expect: run
//! stdout: 3
//! issue: 8899
//! notes: Red on staging r30-c1 (open #8899, over-rejection (3), the out-overwrite shape, same diagnostic). Harvested from the #9075 review (probe a04, rejected on base and tip alike, so not part of the #9075 fix). f first holds the plain function-typed
//!   parameter q (non-empty set). Get(out f) overwrites it, and on normal return the out
//!   location carries the empty set, so Keep(f) at the Object parameter is admitted and the
//!   stored function returns 3, not the callback q.
module Conf;
import System;
public class Reg
  private _o: Object? := null;
  public constructor() end constructor;
  public method Keep(o: Object)
    _o := o;
  end Keep;
  public method Call(): int
    if _o is () -> int then
      return (_o as () -> int)!();
    end if;
    return -1;
  end Call;
end Reg;
public class Program
  public static method Get(out a: () -> int)
    a := lambda(): int => 3;
  end Get;
  public static method Via(reg: Reg, q: () -> int)
    var f: () -> int := q;
    Program.Get(out f);
    reg.Keep(f);
  end Via;
  public static method Main(): int
    using var reg := new Reg();
    var c := 40;
    Program.Via(reg, lambda(): int => c);
    Console.WriteLine(reg.Call());
    return 0;
  end Main;
end Program;

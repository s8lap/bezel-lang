//! case: s28_14_8924
//! doc: spec
//! section: 28.14
//! title: an indexer getter that invokes its index parameter and forwards it to a plain parameter is
//!   admitted
//! rule: Within the callee the callback may be invoked, bound to locals, and passed on as an argument to
//!   another call-bounded parameter
//! cites: 28.14, 23.4
//! expect: run
//! output: 15
//! notes: Admitting twin of 8922/8923: the getter forwards `q` to the plain (call-bounded) parameter of
//!   `Twice` and invokes it once more, which the sentence admits. With q = 5: 5 + 5 + 5 = 15.
//!   Value-only control; green on both arms.
module Conf;
public class Fl
  public constructor() end constructor;
  public static method Twice(f: () -> int): int
    return f() + f();
  end Twice;
  public property [q: () -> int]: int
    get => Fl.Twice(q) + q();
  end;
end Fl;

public class Program
  public static method Main(): int
    var fl := new Fl();
    Console.WriteLine($"{fl[lambda() => 5]}");
    return 0;
  end Main;
end Program;

//! case: s28_14_8915
//! doc: spec
//! section: 28.14
//! title: an indexer getter that invokes its index parameter and returns a capture-free closure is admitted
//! rule: Within the callee the callback may be invoked, bound to locals, and passed on as an argument to
//!   another call-bounded parameter
//! cites: 28.14, 23.4
//! expect: run
//! output: 7
//! notes: Admitting twin of 8914: the getter only invokes `q` (allowed: 'may be invoked') and returns a
//!   fresh capture-free closure that does not mention `q`, so nothing call-bounded escapes. Prints the
//!   closure's result, 7. Value-only control; green on both arms. The lambda argument leaks under the
//!   ASan leak leg (#9174, a lambda argument to a function-valued call); that leak is a separate open
//!   tracker and is not asserted here.
module Conf;
public class Fl
  public constructor() end constructor;
  public property [q: () -> int]: () -> int
    get => do var z := q(); return lambda() => 7; end;
  end;
end Fl;

public class Program
  public static method Main(): int
    var fl := new Fl();
    var f := fl[lambda() => 5];
    Console.WriteLine($"{f()}");
    return 0;
  end Main;
end Program;

//! case: s28_14_8919
//! doc: spec
//! section: 28.14
//! title: an indexer getter that stores a closure capturing its call-bounded index parameter is rejected
//! rule: It may not be stored in a field of any object (a property or indexer assignment is such a store
//! cites: 28.14, 23.4, 23.3
//! expect: reject
//! diagnostic: may not be stored in a field of any object
//! notes: Pins #9084. The closure capturing `q` is stored into the field `_g`, so it escapes the callee.
//!   Twin: 8921 (a setter's `value` is stored, excepted).
module Conf;
public class Fl
  private _g: (() -> int)? := null;
  public constructor() end constructor;
  public property [q: () -> int]: int
    get => do _g := lambda() => q(); return 0; end;
  end;
end Fl;

public class Program
  public static method Main(): int
    var fl := new Fl();
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;

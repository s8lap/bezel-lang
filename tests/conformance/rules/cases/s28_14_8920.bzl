//! case: s28_14_8920
//! doc: spec
//! section: 28.14
//! title: an indexer getter that stores its call-bounded index parameter into a field is rejected
//! rule: It may not be stored in a field of any object (a property or indexer assignment is such a store
//! cites: 28.14, 23.4, 23.3
//! expect: reject
//! diagnostic: may not be stored in a field of any object
//! notes: Pins #9084. `_f := q` stores the call-bounded parameter into a field of the object: 'It may not be
//!   stored in a field of any object'. Twin: 8921 stores the setter's `value`, which is excepted.
module Conf;
public class Fl
  private _f: (() -> int)? := null;
  public constructor() end constructor;
  public property [q: () -> int]: int
    get => do _f := q; return 0; end;
  end;
end Fl;

public class Program
  public static method Main(): int
    var fl := new Fl();
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;

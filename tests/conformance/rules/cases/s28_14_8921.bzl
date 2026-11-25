//! case: s28_14_8921
//! doc: spec
//! section: 28.14
//! title: an indexer setter that stores its `value` function and only invokes the index parameter is
//!   admitted
//! rule: (a setter's `value` and a record's generated-constructor parameter for a function-typed component
//!   excepted
//! cites: 28.14, 23.3, 23.4
//! expect: run
//! output: 40
//! notes: Admitting twin of 8920: a setter's `value` is excepted (it carries the empty source set), so
//!   storing it into `_f` is legal; the call-bounded `q` is only invoked. `CallStored` runs the stored
//!   closure: 40. Value-only control; green on both arms.
module Conf;
public class Fl
  private _f: (() -> int)? := null;
  public constructor() end constructor;
  public property [q: () -> int]: () -> int
    get => lambda() => 5;
    set => do _f := value; var z := q(); end;
  end;
  public method CallStored(): int
    var g := _f;
    if g <> null then return g(); end if;
    return 0;
  end CallStored;
end Fl;

public class Program
  public static method Main(): int
    var fl := new Fl();
    fl[lambda() => 1] := lambda() => 40;
    Console.WriteLine($"{fl.CallStored()}");
    return 0;
  end Main;
end Program;

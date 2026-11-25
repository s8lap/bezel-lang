//! case: s27_5_8715
//! doc: spec
//! section: 27.5
//! title: a virtual indexer setter called with a lambda index argument that captures the receiver
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 27.5, 23.4
//! expect: run
//! output: 200
//!   601
//! notes: Pins #9099 (setter). The index argument `lambda() => fl.Tag` references the receiver
//!   `fl`, a variable of the enclosing scope. Each of the 200 stores runs `Tag := q() + value`
//!   = Tag + 3 (a store is a property access whose index arguments are ordinary expressions,
//!   §23.4): 1 + 3 * 200 = 601; `t` counts the 200 stores.
module Conf;
public class Fl
  public Tag: int;
  public virtual property [q: () -> int]: int
    get => q() + Tag;
    set => do Tag := q() + value; end;
  end;
  public constructor() Tag := 1; end constructor;
end Fl;
public class Program
  public static method Main(): int
    var t := 0;
    var fl := new Fl();
    for var j in 0 ..< 200 do
      fl[lambda() => fl.Tag] := 3;
      t := t + 1;
    end for;
    Console.WriteLine($"{t}");
    Console.WriteLine($"{fl.Tag}");
    return 0;
  end Main;
end Program;

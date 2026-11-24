//! case: s28_3_8003
//! doc: spec
//! section: 28.3
//! title: a nested lambda using an enclosing lambda's owning choice local as a ?? operand
//!   consumes it
//! rule: assignment to it, a marked transfer from it, an explicit `Dispose()` or discard of
//!   it, and every other owned-consuming use of it are compile errors
//! cites: 28.3, 15.3, 20
//! expect: reject
//! diagnostic: captured owned
//! notes: Pins #8987 for the `??` operand position: a `??` operand naming a move-only
//!   binding is unmarked and classified by its position (15.3), i.e. a consume.
module Conf;

public class Res
  public static Count: int := 0;
  public N: int := 7;
  public constructor() end constructor;
  destructor Res.Count := Res.Count + 1; end destructor;
end Res;
public choice OC Text(owned r: Res), Empty; end OC;

public class Program
  public static method Main(): int
    var outer := lambda(): int => do
      var o := OC.Text(new Res());
      var g := lambda(): int => do var x := o ?? OC.Empty; return 1; end;
      var q := g();
      return 0;
    end;
    return outer();
  end Main;
end Program;

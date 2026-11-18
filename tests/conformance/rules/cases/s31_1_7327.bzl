//! case: s31_1_7327
//! doc: spec
//! section: 31.1
//! title: `throw this` inside a lambda in an Exception subclass is rejected
//! rule: so a *borrowed* class reference is not a valid operand
//! cites: 28.3
//! expect: reject
//! diagnostic: OWNED exception
//! notes: Pins the #8781 fix: `this` is a borrow of the receiver, so throwing
//!   it from a lambda body throws a borrowed class reference.
module Conf;

public class R: Exception
  public constructor() end constructor;
end R;

public class Cell
  public value: int := 0;
  public constructor(v: int) this.value := v; end constructor;
end Cell;

public interface IShape method Area(): int; end IShape;

public class Sq: IShape
  public constructor() end constructor;
  public method Area(): int => 4;
end Sq;

public class ER: Exception
  public constructor() end constructor;
  public method Go(): int
    var f := lambda(): int => do throw this; end;
    return f();
  end Go;
end ER;

public class Program
  public static method Main(): int
    using var h := new ER();
    try _ := h.Go(); catch var x: ER => Console.WriteLine("c"); end;
    return 0;
  end Main;
end Program;

//! case: s31_1_7326
//! doc: spec
//! section: 31.1
//! title: a lambda throws its own parameter when the parameter is not marked
//!   `owned`
//! rule: so a *borrowed* class reference is not a valid operand
//! cites: 22.3
//! expect: reject
//! diagnostic: OWNED exception
//! notes: Pins the #8781 fix: the lambda's own parameter `e` has no `owned`
//!   marker, so it is a borrow and `throw e;` is rejected. The accepting twin
//!   is s31_1_7321, which writes `lambda(owned e: R)`.
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

public class Program
  public static method Main(): int
    var f: (R) -> int := lambda(e) => do throw e; end;
    try _ := f(new R()); catch var x: R => Console.WriteLine("caught"); end;
    return 0;
  end Main;
end Program;

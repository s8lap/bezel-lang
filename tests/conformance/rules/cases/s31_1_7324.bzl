//! case: s31_1_7324
//! doc: spec
//! section: 31.1
//! title: a lambda throws a borrowed exception parameter captured from the
//!   enclosing method
//! rule: so a *borrowed* class reference is not a valid operand
//! cites: 28.3
//! expect: reject
//! diagnostic: OWNED exception
//! notes: Pins the #8781 fix: `e` is a plain (borrowed) method parameter
//!   captured by the lambda, so `throw e;` in the lambda body throws a
//!   borrowed class reference. The accepting twin throws a fresh `new R()` in
//!   the same lambda (s31_1_7320 pins the owned-local form).
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

public class T
  public static method Run(e: R): int
    var f := lambda(): int => do throw e; end;
    try _ := f(); catch var x: R => Console.WriteLine("caught"); end;
    return 0;
  end Run;
end T;

public class Program
  public static method Main(): int
    Console.WriteLine(T.Run(new R()));
    return 0;
  end Main;
end Program;

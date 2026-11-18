//! case: s31_1_7321
//! doc: spec
//! section: 31.1
//! title: a lambda with an `owned` parameter throws that parameter
//! rule: `throw expr` requires `expr` to be an owned instance of an
//!   `Exception`-derived class type
//! cites: 22.3
//! expect: run
//! stdout: caught a4b
//! notes: Pins the #8781 fix on the admitting side: an `owned` closure
//!   parameter is an owned operand, so `throw e;` inside the lambda body is
//!   admitted.
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
    var f := lambda(owned e: R): int => do throw e; end;
    try _ := f(new R()); catch var x: R => Console.WriteLine("caught a4b"); end;
    return 0;
  end Main;
end Program;

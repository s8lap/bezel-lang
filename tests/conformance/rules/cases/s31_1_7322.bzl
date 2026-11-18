//! case: s31_1_7322
//! doc: spec
//! section: 31.1
//! title: a bare `throw;` re-raise in a catch arm inside a lambda body is
//!   admitted
//! rule: The two forms are interchangeable inside a catch arm body
//! cites: 21.8, 31.2.4
//! expect: run
//! stdout: caught a5
//! notes: Pins the #8781 fix on the admitting side: the re-raise `throw;`
//!   names no operand, so the borrowed-operand rule does not reach it. The
//!   inner catch arm re-raises and the outer handler sees the R.
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
    var f := lambda(): int => do
      try throw new R(); catch var x: R => throw; end;
      return 0;
    end;
    try _ := f(); catch var y: R => Console.WriteLine("caught a5"); end;
    return 0;
  end Main;
end Program;

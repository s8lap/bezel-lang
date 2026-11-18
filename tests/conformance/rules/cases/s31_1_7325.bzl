//! case: s31_1_7325
//! doc: spec
//! section: 31.1
//! title: an inner lambda throws a borrowed parameter of the enclosing
//!   method, two lambdas deep
//! rule: so a *borrowed* class reference is not a valid operand
//! cites: 28.3
//! expect: reject
//! diagnostic: OWNED exception
//! notes: Pins the #8781 fix through nesting: the throw is in the inner
//!   lambda `i`, and the borrowed parameter `e` belongs to the method around
//!   both lambdas.
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
    var o := lambda(): int => do
      var i := lambda(): int => do throw e; end;
      try _ := i(); catch var x: R => Console.WriteLine("caught"); end;
      return 0;
    end;
    return o();
  end Run;
end T;

public class Program
  public static method Main(): int
    Console.WriteLine(T.Run(new R()));
    return 0;
  end Main;
end Program;

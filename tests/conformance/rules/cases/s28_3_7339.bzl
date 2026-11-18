//! case: s28_3_7339
//! doc: spec
//! section: 28.3
//! title: a lambda body assigns an interface-typed owned local from another
//!   unmarked, in the lambda
//! rule: Assigning a borrow to an owned-state LHS, or an owned value to a
//!   borrowed-state LHS, is rejected
//! cites: 28.8.1
//! expect: reject
//! diagnostic: OWNED state
//! notes: Pins the #8782 fix at an interface-typed LHS: `u := v;` of two
//!   `IShape` owned locals.
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
    var f := lambda(): int => do var u: IShape := new Sq(); var v: IShape := new Sq(); u := v; return u.Area(); end;
    Console.WriteLine(f());
    return 0;
  end Main;
end Program;

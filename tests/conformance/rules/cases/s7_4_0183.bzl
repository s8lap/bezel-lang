//! case: s7_4_0183
//! doc: spec
//! section: 7.4
//! title: a readonly function-typed field test inside the declaring constructor establishes no fact
//! rule: The rule does not apply inside the declaring type's constructors, field initializers, static
//!   constructor, or static field initializers (the assignment window)
//! cites: 22.8, 12.7
//! expect: reject
//! diagnostic: narrow
//! issue: 8099
module Conf;

public class K
  private readonly _f: ((int) -> int)?;

  public constructor()
    _f := lambda(x: int) => x + 1;
    if this._f is (int) -> int then
      Console.WriteLine(this._f(1));
    end if;
  end constructor;
end K;

public class Program
  public static method Main()
    using var k := new K();
  end Main;
end Program;

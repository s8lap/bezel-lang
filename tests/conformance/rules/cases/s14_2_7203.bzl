//! case: s14_2_7203
//! doc: spec
//! section: 14.2
//! title: a type-parameter bound at the IDisposable substitution rejects a
//!   primitive
//! rule: a substitution S for `T` satisfies `where T: U` exactly when S is
//!   non-nullable and is a *subtype of* the substitution for `U`
//! cites: 14.2, 9.7, 28.2
//! expect: reject
//! diagnostic: does not satisfy the constraint
module Conf;

public class D<Y>
  public constructor()
  end constructor;

  public method ZY<S>(a: S): int where S: Y => 1;
end D;

public class Program
  public static method Main()
    var d := new D<IDisposable>();
    Console.WriteLine(d.ZY<int>(3));
  end Main;
end Program;

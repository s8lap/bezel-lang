//! case: s14_2_7201
//! doc: spec
//! section: 14.2
//! title: a type-parameter bound at an interface substitution rejects a
//!   primitive of another instantiation
//! rule: a substitution S for `T` satisfies `where T: U` exactly when S is
//!   non-nullable and is a *subtype of* the substitution for `U`
//! cites: 14.2
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
    var d := new D<IComparable<int>>();
    Console.WriteLine(d.ZY<long>(3L));
  end Main;
end Program;

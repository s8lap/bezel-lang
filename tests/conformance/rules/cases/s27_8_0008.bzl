//! case: s27_8_0008
//! doc: spec
//! section: 27.8
//! title: an alias of an alias on a base return type is overridden by a derived return
//! rule: Type aliases — generic or not — see through to the aliased type for identity, equality, method dispatch, and overload resolution.
//! cites: 27.8, 12.1
//! issue: 6321
//! notes: the RETURN-type site of #6321, and the only row covering it --
//!   s27_8_0012 and friends cover the parameter and index-key sites, where
//!   both sides keep the same stale `T`. MEASURED as a one-fix revert:
//!   reverting only 92135ec1 (#6321), with #6323 present, REJECTS with
//!   "override of `R` does not match the base member's return type (§12.1)";
//!   on the fixed tree this program compiles and prints 42. So #6321 alone
//!   breaks this row.
//! expect: run
//! stdout: 42
module Conf;

type Id<T> := T;
type Id2<T> := Id<T>;

public class B<X>
  public constructor() end constructor;
  public virtual method R(): Id2<X> => default(X);
end B;

public class E: B<int>
  public constructor() end constructor;
  public override method R(): int => 42;
end E;

public class M
  public static method Main(): int
    var e := new E();
    var b: B<int> := e;
    Console.WriteLine(b.R());
    return 0;
  end Main;
end M;

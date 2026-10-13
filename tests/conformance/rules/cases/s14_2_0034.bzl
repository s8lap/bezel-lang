//! case: s14_2_0034
//! doc: spec
//! section: 14.2
//! title: where T: U is not satisfied by nullable wrapping
//! rule: a substitution S for `T` satisfies `where T: U` exactly when S is
//!   non-nullable and is a *subtype of* the substitution for `U` — identical
//!   to it, a class derived from a class substitution (§12), or a type
//!   conforming to an interface substitution ... — and by these three
//!   relations only: no other implicit conversion of §5.1 satisfies the
//!   bound, neither boxing to `Object`, numeric widening, nullable wrapping
//!   nor a variance-induced conversion
//! cites: 14.2, 5.1
//! expect: reject
//! diagnostic: constraint
//! issue: 5906
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Rel<T, U> where T: U
  public constructor()
  end constructor;
end Rel;

public class User
  public static method Run(r: Rel<Widget, Widget?>): int => 1;
end User;

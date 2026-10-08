//! case: s9_7_0026
//! doc: spec
//! section: 9.7
//! title: the same override is admitted when the declaring method bounds the parameter to class
//! rule: A dispatched generic method's body decides such a requirement at the
//!   body (§14.2). ... a bare `T` stored into an `Object`- or interface-typed
//!   field is the same store through §8.6's box at such a substitution.
//! cites: 9.7, 14.2, 8.6
//! expect: accept
module Conf;

public abstract class Base
  public abstract method Keep<U>(x: U) where U: class;
end Base;

public class Der: Base
  private _o: Object?;

  public constructor()
  end constructor;

  public override method Keep<U>(x: U)
    _o := x;
  end Keep;
end Der;

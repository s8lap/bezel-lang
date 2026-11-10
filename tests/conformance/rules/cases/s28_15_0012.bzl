//! case: s28_15_0012
//! doc: spec
//! section: 28.15
//! title: under the report mode, the same generic at a class that is not a carrier reports nothing
//! rule: In a generic body, an owning conversion of a value whose type mentions a
//!   type parameter is a requirement of the body's use of that parameter (§3.6
//!   rule 2): at each instantiation at which the value's type, after
//!   substitution, is a carrier type ... the target type, after substitution,
//!   must be a carrier type, or the instantiation is rejected where it is named.
//!   The requirement is read off the two types; the value's set is not consulted.
//!   — §28.15 *Owned erasure*.
//! cites: 28.3, 3.6
//! expect: quiet
//! flags: --report-trust-admissions
//! issue: 8422
module Conf;

public interface IShow
  method N(): int;
end IShow;

public class Plain: IShow
  public constructor()
  end constructor;

  public method N(): int => 2;
end Plain;

public class Hider
  public static method Hide<T>(owned x: T): owned IShow where T: IShow => x;
end Hider;

public class Program
  public static method Run(): int
    using var s := Hider.Hide<Plain>(new Plain());
    return s.N();
  end Run;
end Program;

//! case: s28_15_0011
//! doc: spec
//! section: 28.15
//! title: under the report mode, a generic owned erasure instantiated at a borrow-bearing class reports
//! rule: In a generic body, an owning conversion of a value whose type mentions a
//!   type parameter is a requirement of the body's use of that parameter (§3.6
//!   rule 2): at each instantiation at which the value's type, after
//!   substitution, is a carrier type ... the target type, after substitution,
//!   must be a carrier type, or the instantiation is rejected where it is named.
//!   The requirement is read off the two types; the value's set is not consulted.
//!   — §28.15 *Owned erasure*. The mode also emits one warning at each site that
//!   is not a store and that §28.15 rejects, naming the rule. (§28.3.12 clause 4)
//! cites: 28.3, 3.6
//! expect: warn
//! diagnostic: carrier
//! flags: --report-trust-admissions
//! issue: 8422
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method N(): int => 1;
end Widget;

public interface IShow
  method N(): int;
end IShow;

public class Tag: IShow
  private _w: Widget? := null;

  public constructor(w: Widget)
  end constructor;

  public method N(): int => 2;
end Tag;

public class Hider
  public static method Hide<T>(owned x: T): owned IShow where T: IShow => x;
end Hider;

public class Program
  public static method Run(): int
    var w := new Widget();
    using var s := Hider.Hide<Tag>(new Tag(w));
    return s.N();
  end Run;
end Program;

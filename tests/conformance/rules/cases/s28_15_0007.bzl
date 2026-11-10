//! case: s28_15_0007
//! doc: spec
//! section: 28.15
//! title: under the report mode, an instance bound over a fresh temporary source reports
//! rule: A fresh unbound owned temporary may not be a source of an instance that
//!   outlives its full expression (K8.4); bind the source first, or let the class
//!   own it through an `owned` parameter. An instance that is itself consumed
//!   within the full expression is admitted. — §28.15 *The bound*. The mode also
//!   emits one warning at each site that is not a store and that §28.15 rejects,
//!   naming the rule. (§28.3.12 clause 4)
//! cites: 28.3, 28.3.11
//! expect: warn
//! diagnostic: temporary
//! flags: --report-trust-admissions
//! issue: 8421
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method N(): int => 1;
end Widget;

public class Tag
  private _w: Widget? := null;

  public constructor(w: Widget)
  end constructor;

  public method N(): int => 2;
end Tag;

public class Program
  public static method Run(): int
    var t := new Tag(new Widget());
    return t.N();
  end Run;
end Program;

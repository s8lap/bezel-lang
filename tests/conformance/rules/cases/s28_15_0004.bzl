//! case: s28_15_0004
//! doc: spec
//! section: 28.15
//! title: under the report mode, a reassignment of a source under a live instance that carries it reports
//! rule: A local that holds the instance is a live local borrow of every source it
//!   carries (K5.5): a transfer or reassignment of a source is frozen while the
//!   local is live. — §28.15 *The bound*; the mint carries each argument "whether
//!   or not the callee's body stores them" (*The source set* clause 1). The mode
//!   also emits one warning at each site that is not a store and that §28.15
//!   rejects, naming the rule. (§28.3.12 clause 4)
//! cites: 28.3
//! expect: warn
//! diagnostic: frozen
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
    var a := new Widget();
    var t := new Tag(a);
    a := new Widget();
    return t.N() + a.N();
  end Run;
end Program;

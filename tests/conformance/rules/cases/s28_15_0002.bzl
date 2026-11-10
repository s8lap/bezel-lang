//! case: s28_15_0002
//! doc: spec
//! section: 28.15
//! title: under the report mode, a transfer of a source after the instance's last use reports nothing
//! rule: A local that holds the instance is a live local borrow of every source it
//!   carries (K5.5): a transfer or reassignment of a source is frozen while the
//!   local is live. — §28.15 *The bound*; the mint carries each argument "whether
//!   or not the callee's body stores them" (*The source set* clause 1). The mode
//!   also emits one warning at each site that is not a store and that §28.15
//!   rejects, naming the rule. (§28.3.12 clause 4) — the local is live to its
//!   last use, and a trivial-dispose instance's release runs no user code
//!   (*Release*)
//! cites: 28.3
//! expect: quiet
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
  public static method Take(owned w: Widget)
  end Take;

  public static method Run(): int
    var a := new Widget();
    var t := new Tag(a);
    var n := t.N();
    Take(move a);
    return n;
  end Run;
end Program;

//! case: s28_15_0009
//! doc: spec
//! section: 28.15
//! title: under the report mode, an owned instance returned at an interface that is not a carrier reports
//! rule: An owned value whose source set is not empty may not reach an **owning
//!   position whose declared type is not a carrier type**. ... an `owned` return
//!   slot ... — §28.15 *Owned erasure*; an interface is borrow-bearing if it
//!   declares or inherits a store member (*The property*). The mode also emits
//!   one warning at each site that is not a store and that §28.15 rejects, naming
//!   the rule. (§28.3.12 clause 4)
//! cites: 28.3
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

public class Program
  public static method Make(w: Widget): owned IShow => new Tag(w);
end Program;

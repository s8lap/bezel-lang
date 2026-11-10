//! case: s28_15_0015
//! doc: spec
//! section: 28.15
//! title: under the report mode, returning a product taken out of a local builder reports
//! rule: An owned value **taken out** of an instance — the `owned` result of one of
//!   its members (clause 1) ... — carries the source of the binding that holds
//!   the instance and the instance's set (§28.15 *The source set* clause 3). An
//!   `owned` return of an instance ... A source that is a scope-bound local, a
//!   `using var`, an `owned` parameter's binding or a temporary is rejected there
//!   (*The bound*, *Returns*). The mode also emits one warning at each site that
//!   is not a store and that §28.15 rejects, naming the rule. (§28.3.12 clause 4)
//! cites: 28.3
//! expect: warn
//! diagnostic: re-establish
//! flags: --report-trust-admissions
//! issue: 8421
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method N(): int => 1;
end Widget;

public class Report
  private _w: Widget? := null;

  public constructor()
  end constructor;

  public method N(): int => 3;
end Report;

public class Builder
  public constructor()
  end constructor;

  public method Build(): owned Report => new Report();
end Builder;

public class Program
  public static method Make(): owned Report
    var b := new Builder();
    var r := b.Build();
    return r;
  end Make;
end Program;

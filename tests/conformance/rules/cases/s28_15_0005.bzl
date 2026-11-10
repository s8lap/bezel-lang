//! case: s28_15_0005
//! doc: spec
//! section: 28.15
//! title: under the report mode, a use of an instance after the disposal of its only source reports
//! rule: An explicit disposal of a source is not frozen (K4.2); a use of the instance
//!   that a disposal of every source dominates is §28.2's compile error. — §28.15
//!   *The bound*. The mode also emits one warning at each site that is not a
//!   store and that §28.15 rejects, naming the rule. (§28.3.12 clause 4)
//! cites: 28.3, 28.2
//! expect: warn
//! diagnostic: disposal
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
    a.Dispose();
    return t.N();
  end Run;
end Program;

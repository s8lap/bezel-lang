//! case: s28_15_0008
//! doc: spec
//! section: 28.15
//! title: under the report mode, an instance over a temporary consumed within its expression reports nothing
//! rule: A fresh unbound owned temporary may not be a source of an instance that
//!   outlives its full expression (K8.4); bind the source first, or let the class
//!   own it through an `owned` parameter. An instance that is itself consumed
//!   within the full expression is admitted. — §28.15 *The bound*.
//! cites: 28.3, 28.3.11
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
  public static method Run(): int
    return new Tag(new Widget()).N();
  end Run;
end Program;

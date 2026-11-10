//! case: s28_15_0006
//! doc: spec
//! section: 28.15
//! title: under the report mode, a use after the disposal of one source of two reports nothing
//! rule: An explicit disposal of a source is not frozen (K4.2); a use of the instance
//!   that a disposal of every source dominates is §28.2's compile error. — §28.15
//!   *The bound*. One source of two disposed is not every source: the use is left
//!   to checked mode's flag.
//! cites: 28.3, 28.2
//! expect: quiet
//! flags: --report-trust-admissions
//! issue: 8421
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method N(): int => 1;
end Widget;

public class Link
  private _a: Widget? := null;
  private _b: Widget? := null;

  public constructor(a: Widget, b: Widget)
  end constructor;

  public method N(): int => 2;
end Link;

public class Program
  public static method Run(): int
    var a := new Widget();
    var b := new Widget();
    var t := new Link(a, b);
    a.Dispose();
    return t.N() + b.N();
  end Run;
end Program;

//! case: s15_3_0014
//! doc: spec
//! section: 15.3
//! title: using var on an owning inline array locks out reassignment
//! rule: For a move-only inline-array local `using var` has no disposal effect,
//!   and its only observable consequence is preventing reassignment — the
//!   spelling for "fixed at construction and may not be rebound" (§15.3,
//!   §28.10).
//! cites: 15.3, 28.10
//! expect: reject
//! diagnostic: reassignment
module Conf;

public class Widget
  public property Tag: string;

  public constructor(tag: string)
    Tag := tag;
  end constructor;
end Widget;

public class Program
  public static method Run()
    using var a: [1 owned]Widget := [new Widget("first")];
    Console.WriteLine(a[0].Tag);
    a := [new Widget("second")];
  end Run;
end Program;

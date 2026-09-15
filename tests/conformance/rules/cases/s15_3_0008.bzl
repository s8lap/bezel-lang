//! case: s15_3_0008
//! doc: spec
//! section: 15.3
//! title: a slot moved-from via move arr[i] and never reassigned is rejected at the exit edge
//! rule: The field-restoration rule applies to individual slots: any slot
//!   moved-from via `move arr[i]` must be reassigned before any exit edge,
//!   because auto-dispose on a moved-from slot is not defined (§15.3).
//! cites: 15.3, 28.8.1
//! expect: reject
//! diagnostic: reassigned
module Conf;

public class Widget
  public property Tag: string;

  public constructor(tag: string)
    Tag := tag;
  end constructor;
end Widget;

public class Program
  public static method Run()
    var a: [1 owned]Widget := [new Widget("first")];
    using var taken := move a[0];
    Console.WriteLine(taken.Tag);
  end Run;
end Program;

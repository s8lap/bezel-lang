//! case: s9_2_0009
//! doc: spec
//! section: 9.2
//! title: equality skip on an explicit field rescues auto-hashable
//! rule: A storage component marked equality skip is removed from the
//!   auto-equality membership computation; a member whose type is inadmissible,
//!   including an explicit field declaration, does not disqualify the record
//!   when marked equality skip.
//! cites: 9.2 (equality skip clause)
//! expect: run
//! stdout: True
module Conf;

public record Counter
  private equality skip _count: int;
  public readonly property Label: string;

  public constructor(label: string, count: int)
    Label := label;
    _count := count;
  end constructor;
end Counter;

public class Program
  public static method Main()
    var a := new Counter("x", 1);
    var b := new Counter("x", 2);
    Console.WriteLine(a = b);
  end Main;
end Program;

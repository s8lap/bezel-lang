//! case: s35_9_0002
//! doc: spec
//! section: 35.9
//! title: default(C?) is a valid null move-only value for an owning C
//! rule: For an owning C, the wrapper C? is itself move-only, and default(C?) is a
//!   valid null move-only value, on the precedent of the owning-inline-array
//!   form.
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public choice Outcome
  Fetched(owned w: Widget),
  Failed;
end Outcome;

public class Program
  public static method Use()
    var o: Outcome? := default(Outcome?);
  end Use;
end Program;

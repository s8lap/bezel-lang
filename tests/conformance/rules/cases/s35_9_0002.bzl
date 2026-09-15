//! case: s35_9_0002
//! doc: spec
//! section: 35.9
//! title: default(C?) is a valid null move-only value for an owning C
//! rule: For an owning C, the wrapper C? is itself move-only, and default(C?) is a
//!   valid null move-only value, on the precedent of §17.11's owning-inline-array
//!   form.
//! cites: 35.9, 17.11, 19.6
//! expect: run
//! stdout: absent
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
  public static method Main()
    var o: Outcome? := default(Outcome?);
    case o of                                     // C? adds the explicit null arm (§19.6)
      null       => Console.WriteLine("absent"),
      Fetched(_) => Console.WriteLine("fetched"),
      Failed     => Console.WriteLine("failed")
    end case;
  end Main;
end Program;

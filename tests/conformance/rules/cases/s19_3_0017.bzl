//! case: s19_3_0017
//! doc: spec
//! section: 19.3
//! title: an unmarked discard slot in an owning match disposes the payload at construct completion
//! rule: Each slot's right-hand side is a full pattern (grammar A.7 `AltPattern`): a
//!   binding (`var w`), `_`, a constant ... in an owning match (§35.5) an owned or
//!   move-only value payload matched by an unmarked named slot is borrowed during
//!   the arm and disposed at construct completion by the standard §35.5
//!   machinery, exactly as a discarded slot
//! cites: 19.3, 35.5
//! expect: run
//! output: discarded
//!   widget disposed
//!   after
//! issue: 2197
module Conf;

public class Widget
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("widget disposed");
  end destructor;
end Widget;

public choice Outcome
  Fetched(owned w: Widget),
  Failed(status: int);
end Outcome;

public class Program
  public static method Main()
    var outcome := Outcome.Fetched(new Widget());
    case move outcome of
      Fetched(w = _) => Console.WriteLine("discarded"),
      Failed(_)      => Console.WriteLine("failed")
    end case;
    Console.WriteLine("after");
  end Main;
end Program;

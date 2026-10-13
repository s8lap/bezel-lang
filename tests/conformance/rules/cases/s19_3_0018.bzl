//! case: s19_3_0018
//! doc: spec
//! section: 19.3
//! title: the transfer marker on a named slot is rejected (the marked slot is retired)
//! rule: A named slot **never transfers** ... `move` is not a slot spelling
//!   (`Fetched(move response = var r)` is a compile error at the pattern naming
//!   the positional form — retired v0.338)
//! cites: 19.3, 35.5
//! expect: reject
//! diagnostic: positional
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
      Fetched(move w = _) => Console.WriteLine("discarded"),
      Failed(_)           => Console.WriteLine("failed")
    end case;
  end Main;
end Program;

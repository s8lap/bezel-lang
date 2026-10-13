//! case: s19_3_0018
//! doc: spec
//! section: 19.3
//! title: a marked named slot with a discard right-hand side is rejected
//! rule: A `_` right-hand side (`move N = _`) is already inadmissible under
//!   condition (iii) — `_` is no binding
//! cites: 19.3, 35.5
//! expect: reject
//! diagnostic: binding
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

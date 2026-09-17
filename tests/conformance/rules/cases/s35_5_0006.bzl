//! case: s35_5_0006
//! doc: spec
//! section: 35.5
//! title: a discarded owned payload is disposed at construct completion
//! rule: Payloads not transferred into bindings — discarded slots like Fetched(_) — are
//!   disposed when the construct completes, by the same unbound-temporary pass
//!   that disposes the container.
//! cites: 35.5, 28.3
//! expect: run
//! output: hit
//!   widget disposed
//!   after
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

public class Client
  public constructor()
  end constructor;

  public method Fetch(): Outcome => Outcome.Fetched(new Widget());
end Client;

public class Program
  public static method Main()
    using var client := new Client();
    case client.Fetch() of
      Fetched(_) => Console.WriteLine("hit"),   // owned payload never bound
      Failed(_)  => Console.WriteLine("other")
    end case;
    Console.WriteLine("after");                 // the payload is gone before this line runs
  end Main;
end Program;

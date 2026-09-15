//! case: s35_5_0002
//! doc: spec
//! section: 35.5
//! title: an owning match is triggered by an unbound owned temporary
//! rule: The owning match is triggered by an unbound owned temporary of owning-choice
//!   classification — a fresh construction, an owning-choice-returning call, a
//!   move result.
//! expect: run
//! stdout: ping
module Conf;

public class Widget
  public method Ping() => Console.WriteLine("ping");

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
    case client.Fetch() of                    // owning match over the fresh temporary
      Fetched(var w) => do
        using var widget := move w;
        widget.Ping();
      end,
      Failed(_) => Console.WriteLine("failed")
    end case;
  end Main;
end Program;

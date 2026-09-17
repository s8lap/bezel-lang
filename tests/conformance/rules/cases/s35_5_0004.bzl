//! case: s35_5_0004
//! doc: spec
//! section: 35.5
//! title: the move marker on a fresh-producer scrutinee is rejected
//! rule: The marker on a borrowed or fresh-producer scrutinee is an error: an unmarked
//!   read never silently consumes, and a fresh producer needs no marker.
//! expect: reject
//! diagnostic: fresh producer
module Conf;

public class Widget
  public constructor()
  end constructor;
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
    case move client.Fetch() of      // ERROR: `move` on a fresh producer, which needs no marker
      Fetched(var w) => Console.WriteLine("ok"),
      Failed(_)      => Console.WriteLine("failed")
    end case;
  end Main;
end Program;

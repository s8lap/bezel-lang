//! case: s35_5_0010
//! doc: spec
//! section: 35.5
//! title: an unmarked named slot never transfers and is disposed at construct completion
//! rule: An unmarked named slot never transfers: a getter may not return a fresh owned
//!   value, so in an owning match an owned payload matched by an unmarked named
//!   slot is borrowed during the arm and disposed at construct completion.
//! expect: run
//! stdout: borrowed
module Conf;

public class Widget
  destructor
    Console.WriteLine("widget disposed");
  end destructor;
end Widget;

public choice Outcome
  Fetched(owned response: Widget),
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
      Fetched(response = var r) => Console.WriteLine("borrowed"),  // unmarked: `r` is a borrow, not transferred
      Failed(_) => Console.WriteLine("failed")
    end case;
  end Main;
end Program;

//! case: s35_5_0010
//! doc: spec
//! section: 35.5
//! title: an unmarked named slot does not transfer, so its binding cannot be moved
//! rule: An unmarked named slot never transfers: in an owning match an owned payload
//!   matched by an unmarked named slot is borrowed during the arm and disposed at
//!   construct completion, exactly as a discarded slot.
//! cites: 35.5, 19.3
//! expect: reject
//! diagnostic: never transfers
module Conf;

public class Widget
  public constructor()
  end constructor;
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
    case client.Fetch() of                     // owning match over the temporary
      Fetched(response = var r) => do
        using var w := move r;                 // ERROR: unmarked slot, so `r` is a borrow
      end,
      Failed(_) => Console.WriteLine("failed")
    end case;
  end Main;
end Program;

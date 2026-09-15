//! case: s35_5_0008
//! doc: spec
//! section: 35.5
//! title: a marked named slot transfers its payload at arm selection
//! rule: A named slot may carry the transfer marker on its source: Fetched(move
//!   response = var r) transfers the payload into r at arm selection, exactly as
//!   the corresponding positional slot would.
//! expect: run
//! stdout: adopted
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
      Fetched(move response = var r) => do
        using var w := move r;
        Console.WriteLine("adopted");
      end,
      Failed(_) => Console.WriteLine("failed")
    end case;
  end Main;
end Program;

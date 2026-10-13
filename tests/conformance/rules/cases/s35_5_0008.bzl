//! case: s35_5_0008
//! doc: spec
//! section: 35.5
//! title: a marked named slot is rejected (the marker is retired at v0.338)
//! rule: A named slot **never transfers** ... `move` is not a slot spelling
//!   (`Fetched(move response = var r)` is a compile error at the pattern naming
//!   the positional form — retired v0.338)
//! cites: 35.5, 19.3
//! expect: reject
//! diagnostic: is not a slot spelling
//! issue: 5938
module Conf;

public class Widget
  public constructor()
  end constructor;

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
        using var w := move r;               // `r` owns the payload: the adopt is admitted
        Console.WriteLine("adopted");
      end,
      Failed(_) => Console.WriteLine("failed")
    end case;
    Console.WriteLine("after");
  end Main;
end Program;

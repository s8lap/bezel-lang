//! case: s28_1_0007
//! doc: spec
//! section: 28.1
//! title: owned choice payload field is admitted
//! rule: The `owned` modifier is admissible on choice payload fields: `Fetched(owned
//!   response: Response)` takes ownership at construction.
//! expect: accept
module Conf;

public class Response
  public constructor()
  end constructor;
end Response;

public choice FetchOutcome
  Fetched(owned response: Response),
  NotModified;
end FetchOutcome;

public class Program
  public method Handle(): FetchOutcome
    var resp := new Response();
    return FetchOutcome.Fetched(move resp);
  end Handle;
end Program;

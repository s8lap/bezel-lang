//! case: s35_3_0007
//! doc: spec
//! section: 35.3
//! title: a named owned binding to an owned payload slot requires the move marker
//! rule: An argument to an owned payload slot is an owned-consuming position: a named
//!   owned binding requires the call-site marker (move resp) and is left
//!   moved-from.
//! expect: reject
//! diagnostic: consumes
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public choice AcquireOutcome
  Ready(owned conn: Connection),
  Empty;
end AcquireOutcome;

public class Program
  public static method Main()
    var c := new Connection();
    var o := AcquireOutcome.Ready(c);   // ERROR: `c` is a named owned binding; write `move c`
  end Main;
end Program;

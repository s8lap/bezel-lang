//! case: s35_4_0011
//! doc: spec
//! section: 35.4
//! title: a choice that holds borrows is forbidden as an async parameter
//! rule: Every choice that holds borrows — whatever its classification — is
//!   subject to section 9.7's borrow-lifetime rules, and is forbidden as an
//!   `async` method's parameter or result (section 30.1).
//! cites: 30.1, 9.7
//! expect: reject
//! diagnostic: holds borrows
module Conf;

public class Logger
  public constructor()
  end constructor;
end Logger;

public choice Report
  Ready(log: Logger),
  Empty;
end Report;

public class Program
  public static async method Handle(r: Report): Task<int>
    return 1;
  end Handle;
end Program;

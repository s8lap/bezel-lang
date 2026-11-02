//! case: s26_5_0057
//! doc: spec
//! section: 26.5
//! title: an undeclared method awaited on a task in an async method is rejected
//! rule: If the receiver's type declares no instance member of that name, the
//!   compiler searches the current module and all imported modules
//! cites: 26.5, 30.3
//! expect: reject
//! diagnostic: Bogus
module Conf;

public class Program
  public static async method Run(): Task<int>
    using var t := Task.FromResult(2);
    await t.Bogus();
    return 1;
  end Run;
end Program;

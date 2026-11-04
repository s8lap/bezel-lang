//! case: s26_5_0057
//! doc: spec
//! section: 26.5
//! title: an undeclared method awaited on a task in an async method is rejected
//! rule: Where neither rule 1 nor this search supplies the name, nothing
//!   supplies it: the access names no member and is a compile error at the
//!   name, the diagnostic naming the receiver's static type.
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

//! case: s35_4_0012
//! doc: spec
//! section: 35.4
//! title: a value-like choice holds no borrows and is admitted as an async parameter
//! rule: A value-like choice never holds borrows (rule 3's construction), so
//!   section 30.1's ban on a borrow-bearing parameter of an `async` method does
//!   not reach it.
//! cites: 30.1
//! expect: accept
module Conf;

public choice Report
  Ready(code: int),
  Empty;
end Report;

public class Program
  public static async method Handle(r: Report): Task<int>
    return (case r of
      Ready(var code) => code,
      Empty           => 0);
  end Handle;
end Program;

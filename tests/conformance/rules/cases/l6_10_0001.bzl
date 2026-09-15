//! case: l6_10_0001
//! doc: stdlib
//! section: 6.10
//! title: TaskScope may not be bound with a plain using var
//! rule: Consumption is await using only — a plain using var, or dropping
//!   the scope undisposed, is a compile error naming this rule: a
//!   synchronous destructor cannot await the drain.
//! cites: 6.10
//! expect: reject
//! diagnostic: await using
module Conf;

import System.Threading;

public class Program
  public method Run()
    using var scope := new TaskScope();
  end Run;
end Program;

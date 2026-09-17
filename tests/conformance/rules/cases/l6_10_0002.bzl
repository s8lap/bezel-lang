//! case: l6_10_0002
//! doc: stdlib
//! section: 6.10
//! title: await using is the admitted way to consume a TaskScope
//! rule: Consumption is await using only — a plain using var, or dropping
//!   the scope undisposed, is a compile error naming this rule.
//! cites: 6.10
//! expect: accept
module Conf;

import System.Threading;

public class Program
  public static async method Run(): Task
    await using var scope := new TaskScope();
    await scope.Join();
  end Run;
end Program;

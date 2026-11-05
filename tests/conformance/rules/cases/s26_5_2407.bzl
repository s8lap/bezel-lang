//! case: s26_5_2407
//! doc: spec
//! section: 26.5
//! title: A user extension Array<T> answers Min() on Array<Array<int>>; the stdlib
//!   candidate is filtered
//! rule: A `where` clause is an *applicability* filter, never a ranking signal.
//! cites: 26.5, 14.2
//! expect: run
//! output: 4242
//! notes: The user block's substituted head Array<Array<int>> is a subtype of the
//!   stdlib head IEnumerable<Array<int>>, so it would win even if the stdlib
//!   candidate applied; green on base as well as on the tip.
module Conf;

extension Array<T>
  public method Min(): int => 4242;
end;

public class Program
  public static method Main(): int
    using var a := new Array<Array<int>>();
    Console.WriteLine(a.Min());
    return 0;
  end Main;
end Program;

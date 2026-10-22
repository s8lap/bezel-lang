//! case: s29_3_0007
//! doc: spec
//! section: 29.3
//! title: a case-arm binding is read after a yield return inside the arm of an iterator method
//! rule: "`yield return expression;` produces the next value and suspends" (§29.3, v0.340), and
//!   "The compiler translates the body into the equivalent state-machine implementation";
//!   the arm's `var n` binding (§19.3) is read again after the first `yield return` resumes,
//!   so the iterator yields `n` then `n + 1`.
//! cites: 19.3, 19.4
//! expect: run
//! output: 7
//!   8
//!   9
module Conf;

public class Program
  public static method G(k: int): IEnumerable<int>
    case k of
      var n when n > 5 => do yield return n; yield return n + 1; end,
      _ => yield return 9;
    end;
  end G;

  public static method Main()
    for var v in G(7) do
      Console.WriteLine(v);
    end;
    for var v in G(3) do
      Console.WriteLine(v);
    end;
  end Main;
end Program;

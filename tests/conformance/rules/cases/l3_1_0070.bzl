//! case: l3_1_0070
//! doc: stdlib
//! section: 3.1
//! title: AppendFormat substitutes a string argument's own text
//! rule: **Contracts.** `AppendFormat`'s format string contains literal text and holes `{n}` — `n` a zero-based index into `args` — each hole optionally carrying a format spec after a colon (`{0:F2}`), interpreted per the §2.18 vocabulary against the argument's runtime type; `{{` and `}}` escape literal braces.
//! cites: 3.1, 2.18, 9.6, 8.5
//! expect: run
//! output: 7
//!   xhelloy
//! issue: 7244
module Conf;

public class Program
  public static method Main(): int
    using var args: Array<Object> := new Array<Object>();
    args.Add("hello");
    using var sb := new StringBuilder();
    sb.AppendFormat("x{0}y", args);
    Console.WriteLine(sb.Length.ToString());
    Console.WriteLine(sb.ToString());
    return 0;
  end Main;
end Program;

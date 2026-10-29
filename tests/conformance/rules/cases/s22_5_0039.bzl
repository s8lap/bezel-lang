//! case: s22_5_0039
//! doc: spec
//! section: 22.5
//! title: reordered named arguments to a generic-context call evaluate in textual order repeatedly
//! rule: Supplied argument expressions are evaluated in call-site textual order, for
//!   named arguments alike, on every evaluation of the call, including inside a
//!   generic method instantiated at several type arguments.
//! cites: 22.5, 14.1
//! expect: run
//! output: 3|AB
//!   3|AB
module Conf;
import System.Text;

public class Program
  static method TD(sb: StringBuilder, s: string, v: double): double
    sb.Append(s);
    return v;
  end TD;
  public static method G<U>(sb: StringBuilder, u: U): int
    return int(Math.Pow(y := TD(sb, "A", 1.0), x := TD(sb, "B", 3.0)));
  end G;

  public static method Main()
    using var sb := new StringBuilder();
    var r := G<int>(sb, 1);
    Console.WriteLine(r.ToString() + "|" + sb.ToString());
    using var sb2 := new StringBuilder();
    var r2 := G<string>(sb2, "q");
    Console.WriteLine(r2.ToString() + "|" + sb2.ToString());
  end Main;
end Program;

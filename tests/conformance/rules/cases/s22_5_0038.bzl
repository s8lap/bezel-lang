//! case: s22_5_0038
//! doc: spec
//! section: 22.5
//! title: nested string.Join with reversed named arguments evaluates in textual order
//! rule: Supplied argument expressions are evaluated left to right as written, for
//!   named arguments too; naming an argument never changes when it runs.
//! cites: 22.5
//! expect: run
//! output: ABC xAxB-CxB-CxBxAxB-CxB-CxBxA
module Conf;
import System.Text;

public class Program
  static method Arr(sb: StringBuilder, s: string): owned Array<string>
    sb.Append(s);
    return new Array<string>(3, "x" + s);
  end Arr;
  static method Sep(sb: StringBuilder, s: string): string
    sb.Append(s);
    return "-" + s;
  end Sep;

  public static method Main()
    using var sb := new StringBuilder();
    var q := string.Join(items := Arr(sb, "A"), separator := string.Join(items := Arr(sb, "B"), separator := Sep(sb, "C")));
    Console.WriteLine(sb.ToString() + " " + q);
  end Main;
end Program;

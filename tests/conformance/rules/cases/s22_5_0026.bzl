//! case: s22_5_0026
//! doc: spec
//! section: 22.5
//! title: Array size and fill named arguments in reverse order evaluate in written order
//! rule: A call's supplied argument expressions are evaluated in call-site textual order, left to right as
//!   written, for positional and named arguments alike; naming an argument changes which parameter it
//!   binds to, never when it runs. `Array<int>(size, fill)` called as
//!   `(fill := e1, size := e2)` runs `e1` first.
//! cites: 22.5, 22.6
//! expect: run
//! output: FS
//!   3 7 7
module Conf;

public class Program
  public static method T(sb: StringBuilder, s: string, v: int): int
    sb.Append(s);
    return v;
  end T;

  public static method Main()
    using var sb := new StringBuilder();
    using var a := new Array<int>(fill := T(sb, "F", 7), size := T(sb, "S", 3));
    Console.WriteLine(sb.ToString());
    Console.WriteLine($"{a.Count} {a[0]} {a[2]}");
  end Main;
end Program;

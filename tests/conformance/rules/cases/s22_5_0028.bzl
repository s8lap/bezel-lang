//! case: s22_5_0028
//! doc: spec
//! section: 22.5
//! title: ArraySpan named Length, Start and Source evaluate in written order
//! rule: A call's supplied argument expressions are evaluated in call-site textual order, left to right as
//!   written, for positional and named arguments alike; naming an argument changes which parameter it
//!   binds to, never when it runs. The positional record
//!   `ArraySpan<T>(Source, Start, Length)` called as `(Length := e1, Start := e2, Source := e3)` runs `e1` first.
//! cites: 22.5, 22.6
//! expect: run
//! output: ls
//!   2 2 3
module Conf;

public class Program
  public static method T(sb: StringBuilder, s: string, v: int): int
    sb.Append(s);
    return v;
  end T;

  public static method Main()
    using var sb := new StringBuilder();
    using var arr := new Array<int>();
    arr.Add(1);
    arr.Add(2);
    arr.Add(3);
    arr.Add(4);
    var sp := new ArraySpan<int>(Length := T(sb, "l", 2), Start := T(sb, "s", 1), Source := arr);
    Console.WriteLine(sb.ToString());
    Console.WriteLine($"{sp.Length} {sp[0]} {sp[1]}");
  end Main;
end Program;

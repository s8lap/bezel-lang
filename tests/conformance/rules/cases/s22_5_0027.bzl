//! case: s22_5_0027
//! doc: spec
//! section: 22.5
//! title: JsonDeserializer named options and input evaluate in written order
//! rule: A call's supplied argument expressions are evaluated in call-site textual order, left to right as
//!   written, for positional and named arguments alike; naming an argument changes which parameter it
//!   binds to, never when it runs. `JsonDeserializer(input, options)` called as
//!   `(options := e1, input := e2)` runs `e1` first.
//! cites: 22.5, 22.6
//! expect: run
//! output: ON
//!   5
module Conf;

import System.Serialization;

public class Program
  public static method TS(sb: StringBuilder, s: string, v: string): string
    sb.Append(s);
    return v;
  end TS;

  public static method TO(sb: StringBuilder, s: string, v: JsonOptions): JsonOptions
    sb.Append(s);
    return v;
  end TO;

  public static method Main()
    using var sb := new StringBuilder();
    var opts := new JsonOptions(false, 2, false, false, 64);
    using var jd := new JsonDeserializer(options := TO(sb, "O", opts), input := TS(sb, "N", "5"));
    Console.WriteLine(sb.ToString());
    Console.WriteLine(jd.ReadInt());
  end Main;
end Program;

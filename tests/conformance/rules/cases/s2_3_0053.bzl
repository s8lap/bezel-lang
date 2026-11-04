//! case: s2_3_0053
//! doc: spec
//! section: 2.3
//! title: a user class named Encoding shadows the auto-imported one and keeps its own GetBytes and GetString
//! rule: every other always-available name may be shadowed by local declarations under the normal name-resolution rules
//! cites: 2.3, 2.7, 3.2
//! expect: run
//! issue: 7028
//! output: 42
//!   u

module Conf;

public class Encoding
  public method GetBytes(s: string): int
    return 42;
  end GetBytes;

  public method GetString(s: string): string
    return "u";
  end GetString;
end Encoding;

public class Program
  public static method Main()
    var e := new Encoding();
    var f: Encoding := e;
    Console.WriteLine(f.GetBytes("x"));
    Console.WriteLine(f.GetString("x"));
  end Main;
end Program;

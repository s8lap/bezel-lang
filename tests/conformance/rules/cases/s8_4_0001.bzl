//! case: s8_4_0001
//! doc: spec
//! section: 8.4
//! title: null equals null under the null-safe equality wrapper
//! rule: `a = b` invokes `a.Equals(b)`, with a null-safe wrapper: `null = null`
//!   is `true`.
//! cites: 8.4
//! expect: run
//! stdout: True
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    var a: Widget? := null;
    var b: Widget? := null;
    Console.WriteLine(a = b);
  end Main;
end Program;

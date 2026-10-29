//! case: s22_5_0068
//! doc: spec
//! section: 22.5
//! title: Path.Combine named arguments written in reverse order are still evaluated left to right
//! rule: A call's **supplied** argument expressions are evaluated in **call-site textual order**
//! cites: 22.5, 7.4
//! expect: run
//! output: eval y
//!   eval x
module Conf;

import System.IO;

public class Util
  public static method F(s: string): string
    Console.WriteLine("eval " + s);
    return s;
  end F;
end Util;

public class Program
  public static method Main()
    _ := Path.Combine(b := Util.F("y"), a := Util.F("x"));
  end Main;
end Program;

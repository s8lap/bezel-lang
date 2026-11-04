//! case: s2_3_0804
//! doc: spec
//! section: 2.3
//! title: locals named Debug, Path and File shadow the imported names when used as expression statements
//! rule: every other always-available name may be shadowed by local declarations under the normal name-resolution rules
//! cites: 2.3, 2.7
//! expect: run
//! output: USERASSERT
//!   PING
//!   PING
//!   PING
//! issue: 8019
//! notes: imports make System.Diagnostics.Debug and System.IO.Path/File visible, so the case is not vacuous: a built-in Debug.Assert(false) would fail.
module Conf;

import System.IO;
import System.Diagnostics;

public class Sh
  public method Combine(a: string, b: string): string
    return "USER:" + a + "+" + b;
  end Combine;
  public method Sqrt(x: double): string
    return "USER";
  end Sqrt;
  public method Ping()
    Console.WriteLine("PING");
  end Ping;
  public method Assert(b: bool)
    Console.WriteLine("USERASSERT");
  end Assert;
  public property Pi: double => 3.25;
end Sh;

public class Program
  public static method Main()
    var Debug := new Sh();
    var Path := new Sh();
    Debug.Assert(false);
    Path.Ping();
    Debug.Ping();
    var File := new Sh();
    File.Ping();
  end Main;
end Program;

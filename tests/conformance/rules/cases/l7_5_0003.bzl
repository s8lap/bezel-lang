//! case: l7_5_0003
//! doc: stdlib
//! section: 7.5
//! title: opening a missing file raises a failure an IFailure arm takes
//! rule: All derive from `IOException`, which derives from `Exception` and
//!   implements `IFailure` (language spec §31.4), so every member of the family
//!   is a failure.
//! cites: 7.5
//! expect: run
//! stdout: True
//! issue: 8414
module Conf;

import System.IO;

public class Program
  public static method Main()
    try
      using var fs := new FileStream("l7_5_0003_absent.txt", FileMode.Open);
      Console.WriteLine("opened");
    catch
      var f: IFailure => Console.WriteLine(f is IOException)
    end;
  end Main;
end Program;

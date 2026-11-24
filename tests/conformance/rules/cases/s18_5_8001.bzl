//! case: s18_5_8001
//! doc: spec
//! section: 18.5
//! title: a null T? interpolation hole at T = string renders the text null
//! rule: When the value is `null` at render time, the hole renders as the four-character
//!   text `null`
//! cites: 18.5, 14.1
//! expect: run
//! issue: 9036
//! stdout: null
//! notes: Red until #9036 is fixed: the executable segfaults on the null path. Found while
//!   writing the #9007 null-argument control (s27_5_8021 tests the null test instead).
//!   No lambda involved.
module Conf;
public class Program
  public static method Use<T>(p: T?): string
    return $"{p}";
  end Use;
  public static method Main(): int
    Console.WriteLine(Program.Use<string>(null));
    return 0;
  end Main;
end Program;

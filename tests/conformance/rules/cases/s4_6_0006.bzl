//! case: s4_6_0006
//! doc: spec
//! section: 4.6
//! title: an open-ended range slice extends to the end of the string
//! rule: s[r] ... the open-ended forms (s[..b], s[a..], s[..]) extend to the
//!   start or end of the string.
//! cites: 4.6
//! expect: run
//! stdout: llo
module Conf;

public class Program
  public static method Main()
    Console.WriteLine("hello"[2..]);
  end Main;
end Program;

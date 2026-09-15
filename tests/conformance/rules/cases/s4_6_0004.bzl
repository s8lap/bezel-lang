//! case: s4_6_0004
//! doc: spec
//! section: 4.6
//! title: a closed range slice includes both endpoints
//! rule: s[r] ... a closed s[a..b] includes both ends.
//! cites: 4.6
//! expect: run
//! stdout: ell
module Conf;

public class Program
  public static method Main()
    Console.WriteLine("hello"[1..3]);
  end Main;
end Program;

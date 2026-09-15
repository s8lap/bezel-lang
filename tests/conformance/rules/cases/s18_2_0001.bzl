//! case: s18_2_0001
//! doc: spec
//! section: 18.2
//! title: the boolean literal true renders as the four-character text True
//! rule: "`true`, `false`, `null`." are the boolean and null literals; the
//!   boolean's rendering is `bool.ToString()`, normative in stdlib §9.4:
//!   "public pure method ToString(): string; // 'True' or 'False'".
//! cites: 18.2
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var flag := true;
    Console.WriteLine(flag);
  end Main;
end Program;

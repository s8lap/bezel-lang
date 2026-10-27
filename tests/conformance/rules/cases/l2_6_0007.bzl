//! case: l2_6_0007
//! doc: stdlib
//! section: 2.6
//! title: Guid.ToString("D") renders the hyphenated lowercase D form
//! rule: | `Guid` | `D` (hyphenated, lowercase), `N` (no hyphens) | no-format = `D` |
//! cites: 2.6, 2.18
//! expect: run
//! stdout: 00000000-0000-0000-0000-000000000000

module Conf;

public class Program
  public static method Main()
    var g: Guid := Guid.Empty;
    Console.WriteLine(g.ToString("D"));
  end Main;
end Program;

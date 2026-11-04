//! case: s30_3_0038
//! doc: spec
//! section: 30.3
//! title: Reading Task.IsFaulted as a property is legal
//! rule: public readonly property IsFaulted: bool;
//! cites: 30.3
//! expect: run
//! stdout: False
//! issue: 7971
//! notes: twin of s30_3_0037 (pairs the accepted with its rejecting form).
module Conf;

import System.Threading.Tasks;

public class Program
  public static method Main()
    using var t := Task.FromResult(2);
    Console.WriteLine(t.IsFaulted);
  end Main;
end Program;

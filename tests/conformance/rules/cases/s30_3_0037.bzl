//! case: s30_3_0037
//! doc: spec
//! section: 30.3
//! title: Calling Task.IsFaulted as a method is rejected
//! rule: public readonly property IsFaulted: bool;
//! cites: 30.3, 26.5
//! expect: reject
//! diagnostic: is a property of
//! issue: 7971
//! notes: twin of s30_3_0038 (pairs the rejected with its accepting form).
module Conf;

import System.Threading.Tasks;

public class Program
  public static method Main()
    using var t := Task.FromResult(2);
    var a := t.IsFaulted();
  end Main;
end Program;

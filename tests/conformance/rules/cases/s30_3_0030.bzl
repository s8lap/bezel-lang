//! case: s30_3_0030
//! doc: spec
//! section: 30.3
//! title: Calling Result without a call-through-property on an int task is rejected
//! rule: public readonly property Result: T;
//! cites: 30.3, 26.5
//! expect: reject
//! diagnostic: is a property of
//! issue: 7971
//! notes: twin of s30_3_0029 (pairs the rejected with its accepting form).
module Conf;

import System.Threading.Tasks;

public class Program
  public static method Main()
    using var t := Task.FromResult(2);
    var v := t.Result();
    Console.WriteLine(v.ToString());
  end Main;
end Program;

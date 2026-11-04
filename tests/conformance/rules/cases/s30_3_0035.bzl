//! case: s30_3_0035
//! doc: spec
//! section: 30.3
//! title: An alias of Task of int does not make Result callable
//! rule: public readonly property Result: T;
//! cites: 27.8, 30.3
//! expect: reject
//! diagnostic: is a property of
//! issue: 7971
//! notes: twin of s30_3_0034 (pairs the rejected with its accepting form).
module Conf;

import System.Threading.Tasks;

type TI := Task<int>;

public class Program
  public static method Main()
    using var t: TI := Task.FromResult(2);
    var v := t.Result();
    Console.WriteLine(v.ToString());
  end Main;
end Program;

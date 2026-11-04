//! case: s30_3_0036
//! doc: spec
//! section: 30.3
//! title: Result of a type-parameter-typed Task is not callable
//! rule: public readonly property Result: T;
//! cites: 30.3, 27.1
//! expect: reject
//! diagnostic: is a property of
//! issue: 7971
//! notes: twin of s30_3_0029 (pairs the rejected with its accepting form).
module Conf;

import System.Threading.Tasks;

public class Program
  public static method F<T>(t: Task<T>)
    var v := t.Result();
  end F;

  public static method Main()
    using var t := Task.FromResult(2);
    F<int>(t);
  end Main;
end Program;

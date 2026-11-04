//! case: s30_3_0034
//! doc: spec
//! section: 30.3
//! title: An alias of Task of a function type keeps Result callable
//! rule: whose callee is a function-typed value — a parameter, local, field, or
//!   property of function type, or any expression of one
//! cites: 27.8, 30.3
//! expect: run
//! stdout: 7
//! issue: 7971
//! notes: twin of s30_3_0035 (pairs the accepted with its rejecting form).
module Conf;

import System.Threading.Tasks;

type TF := Task<() -> int>;

public class Program
  public static method Main()
    using var t: TF := Task.FromResult<() -> int>(lambda() => 7);
    var v := t.Result();
    Console.WriteLine(v.ToString());
  end Main;
end Program;

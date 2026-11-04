//! case: s30_3_0031
//! doc: spec
//! section: 30.3
//! title: A parenthesised Task result of function type is callable
//! rule: whose callee is a function-typed value — a parameter, local, field, or
//!   property of function type, or any expression of one
//! cites: 27.1, 27.7, 30.3
//! expect: run
//! stdout: 7
//! issue: 7971
//! notes: twin of s30_3_0030 (pairs the accepted with its rejecting form).
module Conf;

import System.Threading.Tasks;

public class Program
  public static method Main()
    using var t := Task.FromResult<() -> int>(lambda() => 7);
    var v := (t.Result)();
    Console.WriteLine(v.ToString());
  end Main;
end Program;

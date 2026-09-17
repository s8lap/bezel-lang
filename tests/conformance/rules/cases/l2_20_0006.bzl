//! case: l2_20_0006
//! doc: stdlib
//! section: 2.20
//! title: UnwrapOr returns the fallback on Error
//! rule: UnwrapOr(fallback) returns v on Ok(v) and fallback on Error(_).
//! cites: 2.20
//! expect: run
//! stdout: 99
module Conf;

public class Program
  public static method Main()
    var r: Result<int, string> := Result<int, string>.Error("bad");
    var v := r.UnwrapOr(99);
    Console.WriteLine(v);
  end Main;
end Program;

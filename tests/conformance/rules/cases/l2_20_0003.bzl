//! case: l2_20_0003
//! doc: stdlib
//! section: 2.20
//! title: Map passes Error through unchanged
//! rule: Map(f) passes Error(e) through unchanged.
//! cites: 2.20
//! expect: run
//! stdout: bad
module Conf;

public class Program
  public static method Main()
    var r: Result<int, string> := Result<int, string>.Error("bad");
    var r2 := r.Map(lambda(x) => x * 2);
    case r2 of
      Ok(var v) => Console.WriteLine(v),
      Error(var e) => Console.WriteLine(e)
    end case;
  end Main;
end Program;

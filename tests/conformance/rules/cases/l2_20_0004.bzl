//! case: l2_20_0004
//! doc: stdlib
//! section: 2.20
//! title: MapError transforms the Error payload
//! rule: MapError(f) ... maps Error(e) to Error(f(e)).
//! cites: 2.20
//! expect: run
//! stdout: 3
module Conf;

public class Program
  public static method Main()
    var r: Result<int, string> := Result<int, string>.Error("bad");
    var r2 := r.MapError(lambda(e) => e.Length);
    case r2 of
      Ok(var v) => Console.WriteLine(v),
      Error(var e) => Console.WriteLine(e)
    end case;
  end Main;
end Program;

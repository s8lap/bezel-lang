//! case: l2_20_0002
//! doc: stdlib
//! section: 2.20
//! title: Map transforms the Ok payload
//! rule: Map(f) maps Ok(v) to Ok(f(v)).
//! cites: 2.20
//! expect: run
//! stdout: 10
module Conf;

public class Program
  public static method Main()
    var r: Result<int, string> := Result<int, string>.Ok(5);
    var r2 := r.Map(lambda(x) => x * 2);
    case r2 of
      Ok(var v) => Console.WriteLine(v),
      Error(var e) => Console.WriteLine(e)
    end case;
  end Main;
end Program;

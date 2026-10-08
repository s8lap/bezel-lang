//! case: l2_20_0005
//! doc: stdlib
//! section: 2.20
//! title: AndThen sequences a second fallible step on Ok
//! rule: AndThen(f) ... returns f(v) on Ok(v) and passes Error(e) through.
//! cites: 2.20, 35.3
//! expect: run
//! stdout: 5
module Conf;

public class Program
  public static method Main()
    var r: Result<int, string> := Result<int, string>.Ok(10);
    var r2 := r.AndThen(lambda(x): Result<int, string> => do
      if x mod 2 = 0 then return Ok(x / 2); end;
      return Error("odd");
    end);
    case r2 of
      Ok(var v) => Console.WriteLine(v),
      Error(var e) => Console.WriteLine(e)
    end case;
  end Main;
end Program;

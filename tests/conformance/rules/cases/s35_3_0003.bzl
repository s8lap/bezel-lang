//! case: s35_3_0003
//! doc: spec
//! section: 35.3
//! title: a bare case construction in a declared-result position resolves against the declared type
//! rule: In a declared-result position — the top-level operand of a return statement in
//!   a body with a declared return type — where the declared type is a choice
//!   instantiation that declares a case named N, the form N(args) is a case
//!   construction.
//! expect: run
//! stdout: out of range: 150
module Conf;

public class Program
  public static method ParsePercent(value: int): Result<int, string>
    if value < 0 or value > 100 then
      return Error($"out of range: {value}");
    end if;
    return Ok(value);
  end ParsePercent;

  public static method Main()
    case ParsePercent(150) of
      Ok(var v)    => Console.WriteLine($"ok {v}"),
      Error(var m) => Console.WriteLine(m)
    end case;
  end Main;
end Program;

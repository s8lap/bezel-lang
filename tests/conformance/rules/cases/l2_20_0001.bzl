//! case: l2_20_0001
//! doc: stdlib
//! section: 2.20
//! title: IsOk() is true for the Ok case
//! rule: IsOk() distinguishes Ok from Error by case, compiler-provided.
//! cites: 2.20, 35.3
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var r: Result<int, string> := Result<int, string>.Ok(5);
    Console.WriteLine(r.IsOk());
  end Main;
end Program;

//! case: s35_3_0024
//! doc: spec
//! section: 35.3
//! title: an async method returning a choice-bearing Task builds bare cases by return and expression body
//! rule: "for an `async` body, the declared `Task<T>`'s result type, per §30.1's completion rule" is the declared type (§35.3, v0.340): `Later` returns `Error(...)` or `Ok(...)` bare as `Task<Result<string, string>>`, and `Fe(n) => Ok(n)` is an expression body. For n = 0..3: `Error("even0")` -> 1, `Ok("odd1")` -> 4, 1, 4, total 10; `Fe(7)` is `Ok(7)`.
//! cites: 30.1
//! expect: run
//! output: 10
//!   7
module Conf;

public class Box
  public static async method Later(n: int): Task<Result<string, string>>
    if n mod 2 = 0 then return Error("even" + n.ToString()); end;
    return Ok("odd" + n.ToString());
  end Later;

  public static async method Fe(n: int): Task<Result<int, string>> => Ok(n);
end Box;

public class Program
  public static async method Main(): Task<int>
    var tot := 0;
    for var i in 0..3 do
      var t := await Box.Later(i);
      tot := tot + (case t of Ok(var v) => v.Length, Error(var e) => 1);
    end;
    Console.WriteLine(tot);
    var f := await Box.Fe(7);
    Console.WriteLine((case f of Ok(var v) => v, Error(var e) => -1));
    return 0;
  end Main;
end Program;

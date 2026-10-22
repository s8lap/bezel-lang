//! case: s35_3_0023
//! doc: spec
//! section: 35.3
//! title: an ascribed lambda return in a declared-result position constructs bare cases
//! rule: "a `return e;` statement or expression body checked against a lambda's return-type ascription (§27.3)" is a declared-result position (§35.3, v0.340): the lambdas ascribed `Result<..>` construct `Ok(...)`/`Error(...)` bare, in the expression body and in the block body. The four iterations (4..7) give ids 4+5+6+7 = 22 and string lengths 2, 2, then `Error` -> 100, 100.
//! cites: 27.3, 35.4
//! expect: run
//! output: 4 226
module Conf;

public class Res
  public static _n: int := 0;
  public id: int;

  public constructor(i: int)
    id := i;
  end constructor;

  destructor
    _n := _n + 1;
  end destructor;
end Res;

public class Program
  public static method Main()
    var f := lambda(x: int): Result<Res, string> => Ok(new Res(x));
    var g := lambda(x: int): Result<string, string> => do
      if x > 5 then return Error("big" + x.ToString()); end;
      return Ok("s" + x.ToString());
    end;
    var k := 0;
    for var i in 4..7 do
      using var r := f(i);
      k := k + (case r of Ok(var x) => x.id, Error(var e) => 0);
      var s := g(i);
      k := k + (case s of Ok(var v) => v.Length, Error(var e) => 100);
    end;
    Console.WriteLine($"{Res._n} {k}");
  end Main;
end Program;

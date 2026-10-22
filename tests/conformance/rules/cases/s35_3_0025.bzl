//! case: s35_3_0025
//! doc: spec
//! section: 35.3
//! title: a generic method returning a generic choice builds an owned payload bare and frees it
//! rule: "Payload-argument binding and ownership are exactly the qualified form's" (§35.3, v0.340): `Ok(move x)` in `Wrap<T>(x: owned T): Result<T, string>` transfers the owned payload into the choice, which owns it, so the destructor of each `Res` runs once when `r` leaves its scope: for i = 0..2 the count is 3 and `k` = 3 x (1 + 3) + (0 + 1 + 2) = 15.
//! cites: 35.4, 28.3
//! expect: run
//! output: 3 15
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

public class Box
  public static method Wrap<T>(x: owned T): Result<T, string>
    return Ok(move x);
  end Wrap;

  public static method Fail<T>(): Result<T, string> => Error("bad");
end Box;

public class Program
  public static method Main()
    var k := 0;
    for var i in 0..2 do
      using var r := Box.Wrap<Res>(new Res(i));
      k := k + (case r of Ok(var x) => 1, Error(var e) => 0);
      using var f := Box.Fail<Res>();
      k := k + (case f of Ok(var x) => 1000, Error(var e) => e.Length);
      var s := Box.Wrap<int>(i);
      k := k + (case s of Ok(var v) => v, Error(var e) => 0);
    end;
    Console.WriteLine($"{Res._n} {k}");
  end Main;
end Program;

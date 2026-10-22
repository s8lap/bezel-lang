//! case: s35_3_0019
//! doc: spec
//! section: 35.3
//! title: a nullable declared choice result target-types through the stripped form
//! rule: "A declared nullable `C<…>?` target-types through the stripped form and the result converts by §5.1's `T → T?` wrapping" (§35.3, v0.340): `Ok(n)` and `Error("neg")` in a `Result<int, string>?` result are `Result<int, string>` cases wrapped to the nullable type, and `null` stays null.
//! cites: 5.1
//! expect: run
//! output: null
//!   neg
//!   ok 5
//!   q 9
module Conf;

public class Program
  public static method P(n: int): Result<int, string>?
    if n = 0 then return null; end;
    if n < 0 then return Error("neg"); end;
    return Ok(n);
  end P;

  public static method Q(): Result<int, string>? => Ok(9);

  public static method Main()
    for var i in [0, -2, 5] do
      var r := P(i);
      if r <> null then
        Console.WriteLine((case r of Ok(var v) => $"ok {v}", Error(var e) => e));
      else Console.WriteLine("null"); end;
    end;
    var q := Q();
    if q <> null then Console.WriteLine((case q of Ok(var v) => $"q {v}", Error(var e) => e)); end;
  end Main;
end Program;

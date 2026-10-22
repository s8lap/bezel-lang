//! case: s35_3_0015
//! doc: spec
//! section: 35.3
//! title: a parameter or local named like the choice type does not disturb the declared-result case reading
//! rule: "where the declared type — nullability stripped — is a choice instantiation `C<…>` (or non-generic choice `C`) that declares a case named `N`: the form `N(args)` is a case construction of `C<…>.N(args)`, and the bare identifier `N` is the payload-free case value `C<…>.N`" (§35.3, v0.340). The parameter `Fetch: int` and the local `Result := 4` have the names of the choice types; the cases `NotModified`, `Got` and `Ok` are still constructed, and the payload `Ok(Result)` reads the local.
//! cites: 35.5
//! expect: run
//! output: nm
//!   n2
//!   4
module Conf;

public choice Fetch
  Got(body: string),
  NotModified;
end Fetch;

public class Program
  public static method P(Fetch: int): Fetch
    if Fetch = 0 then return NotModified; end;
    return Got($"n{Fetch}");
  end P;

  public static method Q(): Result<int, string>
    var Result := 4;
    return Ok(Result);
  end Q;

  public static method Main()
    Console.WriteLine((case P(0) of Fetch.Got(var b) => b, Fetch.NotModified => "nm"));
    Console.WriteLine((case P(2) of Fetch.Got(var b) => b, Fetch.NotModified => "nm"));
    Console.WriteLine((case Q() of Ok(var v) => $"{v}", Error(var e) => e));
  end Main;
end Program;

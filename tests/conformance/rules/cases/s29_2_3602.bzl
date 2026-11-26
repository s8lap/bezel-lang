//! case: s29_2_3602
//! doc: spec
//! section: 29.2
//! title: a closure over an owned frame local yielded from an iterator is rejected even with no later move
//! rule: a function value is judged at the `yield return` itself by the closure bullet's sink rule,
//!   exactly as `return f;` is at a return (`var f := lambda(): int => a.N; yield return f;`)
//! cites: 29.2, 28.3.8
//! expect: reject
//! diagnostic: yield
//! issue: 9088
module Conf;

public class Res
  public N: int := 7;
  public constructor() end constructor;
end Res;

public class Program
  public static method Gen(): IEnumerable<() -> int>
    for var i in 0..3 do
      using var a := new Res();
      var f := lambda(): int => a.N;
      yield return f;
    end;
  end Gen;
end Program;

//! case: s28_3_9_8503
//! doc: spec
//! section: 28.3.9
//! title: a closure called in a `yield return` after the move of its owner is rejected, nested
//!   iterator
//! rule: A borrow is *live* at a consume site when some path from the site reaches a later use of
//!   it
//! cites: 28.3.9, 28.3
//! expect: reject
//! diagnostic: cannot consume
//! notes: Pins #9078 (first shape): the closure value `f` holds a borrow of `a`; a `yield return
//!   f()` that follows `move a` is a later use of that borrow reached from the consume site.
//!   The inner iterator is consumed by an outer one; the rule is checked in the inner body.
//!   Twin: s28_3_9_8504.
module Conf;
public class Res
  public N: int := 7;
  public constructor() end constructor;
end Res;
public class Sink
  public static method Take(owned r: Res): int => 1;
end Sink;

public class Program
  public static method Inner(): IEnumerable<int>
    var a := new Res();
    var f := lambda(): int => a.N;
    var t := Sink.Take(move a);
    yield return f();
  end Inner;
  public static method Gen(): IEnumerable<int>
    for var v in Inner() do yield return v; end;
  end Gen;
  public static method Main(): int
    var r := 0;
    for var x in Gen() do r := r + x; end;
    Console.WriteLine($"{r}");
    return 0;
  end Main;
end Program;

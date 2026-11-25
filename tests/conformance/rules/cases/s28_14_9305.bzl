//! case: s28_14_9305
//! doc: spec
//! section: 28.14
//! title: a lazy query that does not touch the callback may be captured by a returned closure
//! rule: a frame that carries the callback and escapes any other way
//! cites: 28.14, 28.3, 29.11
//! expect: run
//! output: 9
//! notes: Accepting twin of s28_14_9303 (differs only in `x + 1` for `x + q()`): the query no longer carries
//!   the callback, `q` is not used at all, so nothing call-bounded escapes. Trace: the query is
//!   1, 2, 3 each plus 1 = 2, 3, 4; the returned closure sums it once: 9.
module Conf;
public class Fl
  public constructor() end constructor;
  public method Get(q: () -> int): () -> int
    var s := (1..3).Select(lambda(x: int) => x + 1);
    return lambda() => s.Sum();
  end Get;
end Fl;
public class Program
  public static method Main(): int
    using var fl := new Fl();
    var k := 5;
    var f := fl.Get(lambda() => k);
    Console.WriteLine(f());
    return 0;
  end Main;
end Program;

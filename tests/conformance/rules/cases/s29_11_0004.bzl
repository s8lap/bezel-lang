//! case: s29_11_0004
//! doc: spec
//! section: 29.11
//! title: applying a further deferred operator spends the query's enumeration right
//! rule: Applying a further deferred operator: `var q2 := q.Select(f);`
//!   spends the binding `q`'s enumeration right — using `q` afterwards as a
//!   query is a static error where visible.
//! cites: 29.11
//! expect: reject
//! diagnostic: ToArray
module Conf;

public class Program
  public static method Main()
    var xs: Array<int> := [1, 2, 3];
    using var q := xs.Where(lambda(x) => x > 1);
    using var q2 := q.Select(lambda(x) => x * 2);
    for var v in q do
      _ := v;
    end;
  end Main;
end Program;

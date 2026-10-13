//! case: s29_8_0027
//! doc: spec
//! section: 29.8
//! title: a second GetEnumerator call on an owned-parameter iterable is a compile error
//! rule: its enumeration right is spent at the first consumption point (a
//!   `for` or `await for`, a terminal operator, a `GetEnumerator()` or
//!   `GetAsyncEnumerator()` call or its owning form, ...); a second
//!   consumption is a compile error where the flow analysis can see it
//! cites: 29.8, 29.11
//! expect: reject
//! diagnostic: ToArray
//! issue: 5824
module Conf;

public class Lines
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("closed");
  end destructor;
end Lines;

public class Program
  public static method Echo(owned source: Lines): owned IEnumerable<int>
    yield return 1;
  end Echo;

  public static method Main()
    using var xs := Echo(new Lines());
    using var e1 := xs.GetEnumerator();
    using var e2 := xs.GetEnumerator();
  end Main;
end Program;

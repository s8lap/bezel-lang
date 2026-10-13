//! case: s27_6_0009
//! doc: spec
//! section: 27.6
//! title: a method reference over a generic method with no type-argument list is rejected
//! rule: A method reference over a **generic method** — one declaring type
//!   parameters of its own — names its type arguments on the member name
//!   (`Util.Keep<Pair>`, `s.Put<[2 owned]Item>`, `xs.Select(Util.Id<int>)`),
//!   under §14.5's all-or-nothing rule for explicit lists; a reference to a
//!   generic method with no list is a compile error naming the type
//!   parameters to supply, and no inference binds them from the target.
//! cites: 27.6, 14.5
//! expect: reject
//! diagnostic: type argument
//! issue: 5814
module Conf;

public class Util
  public static method Id<T>(x: T): T => x;

  public static method Run(): int
    var f: (int) -> int := Util.Id;
    return f(3);
  end Run;
end Util;

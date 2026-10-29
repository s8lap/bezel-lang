//! case: s14_2_0091
//! doc: spec
//! section: 14.2
//! title: a nullable type argument does not satisfy an interface bound, as Math.Max<int?> shows
//! rule: "a class or interface bound admits only non-nullable substitutions, as `where T: class`
//!   does" (§14.2, v0.350). `Math.Max<T>(a: T, b: T): T where T: IComparable<T>` (stdlib §2.2,
//!   v0.350) names an interface bound, so the reference `Math.Max<int?>` is rejected at its
//!   formation (§27.6, v0.350: a method reference formation is an instantiation site).
//! cites: 14.2, 27.6
//! expect: reject
//! diagnostic: does not satisfy the constraint
module Conf;

public class Program
  public static method Main()
    var m: (int?, int?) -> int? := Math.Max<int?>;
  end Main;
end Program;

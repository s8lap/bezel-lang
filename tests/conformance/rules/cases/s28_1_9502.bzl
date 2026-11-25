//! case: s28_1_9502
//! doc: spec
//! section: 28.1
//! title: a discard of an owned parameter of the enclosing type's T is a no-op at value substitutions
//! rule: marker-keyed call-site and binding forms written under the class assumption remain admissible
//!   with no-op effect
//! cites: 28.1, 17.13
//! expect: run
//! output: q=400
//! notes: Spec-decided control of the #9221 fix (pick 21cc495b2), enclosing-type form of s28_1_9501: 2
//!   calls * 200 iterations, each returning 1: 400.
module Conf;

public class H<T>
  public static method Use(owned x: T): int
    _ := x;
    return 1;
  end Use;
end H;

public class Program
  public static method Main(): int
    var q := 0;
    var i := 0;
    while i < 200 do
      q := q + H<int>.Use(5);
      q := q + H<string>.Use("x");
      i := i + 1;
    end while;
    Console.WriteLine($"q={q}");
    return 0;
  end Main;
end Program;

//! case: s28_1_9501
//! doc: spec
//! section: 28.1
//! title: a discard of an owned bare-T parameter is a no-op at value and runtime-managed substitutions
//! rule: marker-keyed call-site and binding forms written under the class assumption remain admissible
//!   with no-op effect
//! cites: 28.1, 17.13
//! expect: run
//! output: q=600
//! notes: Spec-decided control of the #9221 fix (pick 21cc495b2): the `_ := x;` written under the class
//!   assumption is admissible with no-op effect at an int, a string and a record substitution (the
//!   marker is erased); the base ran the class-assumed discard on a non-pointer value, a
//!   sanitizer-visible defect. 3 calls * 200 iterations, each returning 1: 600.
module Conf;

public record P(A: int, B: string);

public class G
  public static method Use<T>(owned x: T): int
    _ := x;
    return 1;
  end Use;
end G;

public class Program
  public static method Main(): int
    var q := 0;
    var i := 0;
    while i < 200 do
      q := q + G.Use<int>(5);
      q := q + G.Use<string>("x");
      q := q + G.Use<P>(new P(1, "s"));
      i := i + 1;
    end while;
    Console.WriteLine($"q={q}");
    return 0;
  end Main;
end Program;

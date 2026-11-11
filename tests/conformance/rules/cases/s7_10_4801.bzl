//! case: s7_10_4801
//! doc: spec
//! section: 7.10
//! title: ?? over an unconstrained type-parameter left operand is admitted and selects by nullness
//! rule: Inside an unconstrained generic body, `T` is treated conservatively as possibly nullable.
//! cites: 7.10, 7.7, 14.2
//! expect: run
//! output: 3
//!   d
module Conf;

public class G
  public static method Pick<T>(x: T, d: T): T
    return x ?? d;
  end Pick;
end G;

public class Program
  public static method Main()
    Console.WriteLine(G.Pick<int>(3, 4));
    var r: string? := G.Pick<string?>(null, "d");
    Console.WriteLine(r!);
  end Main;
end Program;

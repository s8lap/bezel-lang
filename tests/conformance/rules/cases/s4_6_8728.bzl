//! case: s4_6_8728
//! doc: spec
//! section: 4.6
//! title: a generic lambda result at a nullable string substitution is bound and read like any
//!   string
//! rule: An **instantiation** of a generic declaration is the constructed type or constructed member obtained by substituting type arguments for its type parameters
//! cites: 4.6, 28.9, 14.1
//! expect: run
//! stdout: 1090
//! notes: Pins #9118. `M<string?>` returns a closure over `p`; its result is bound with `var r` and
//!   read. A string's memory is reclaimed automatically, so the program only reads the
//!   strings. The lengths of "fr-0" .. "fr-199" are 4 for j < 10, 5 for 10 <= j < 100 and 6
//!   for 100 <= j < 200: 40 + 450 + 600 = 1090.
module Conf;
public class Program
  public static method M<T>(p: T): () -> T
    return lambda(): T => p;
  end M;
  public static method Run(): int
    var t := 0;
    for var j in 0 ..< 200 do
      var f := Program.M<string?>($"fr-{j}");
      var r := f();
      t := t + (r ?? "").Length;
    end for;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine($"{Program.Run()}");
    return 0;
  end Main;
end Program;

//! case: s4_6_8730
//! doc: spec
//! section: 4.6
//! title: a generic lambda result at a nullable string substitution is returned from a callee
//! rule: An **instantiation** of a generic declaration is the constructed type or constructed member obtained by substituting type arguments for its type parameters
//! cites: 4.6, 28.9, 14.1
//! expect: run
//! stdout: 1090
//! notes: Pins #9118. `Take` returns the closure's `string?` result to its caller. The lengths of
//!   "fr-0" .. "fr-199" are 4 for j < 10, 5 for 10 <= j < 100 and 6 for 100 <= j < 200: 40 +
//!   450 + 600 = 1090.
module Conf;
public class Program
  public static method M<T>(p: T): () -> T
    return lambda(): T => p;
  end M;
  public static method Take(j: int): string?
    var f := Program.M<string?>($"fr-{j}");
    return f();
  end Take;
  public static method Run(): int
    var t := 0;
    for var j in 0 ..< 200 do
      var r := Program.Take(j);
      t := t + (r ?? "").Length;
    end for;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine($"{Program.Run()}");
    return 0;
  end Main;
end Program;

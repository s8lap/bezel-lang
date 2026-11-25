//! case: s4_6_8732
//! doc: spec
//! section: 4.6
//! title: a generic lambda result at a nullable string substitution is stored in a string field
//! rule: `string` is **runtime-managed**: its memory is reclaimed automatically.
//! cites: 4.6, 28.9, 14.1
//! expect: run
//! stdout: 1090
//! notes: Pins #9118 (field store). The closure result is stored into a `string?` field of `Holder`
//!   and read back through the field. The lengths of "fr-0" .. "fr-199" are 4 for j < 10, 5
//!   for 10 <= j < 100 and 6 for 100 <= j < 200: 40 + 450 + 600 = 1090.
module Conf;
public class Holder
  public F: string?;
end Holder;
public class Program
  public static method M<T>(p: T): () -> T
    return lambda(): T => p;
  end M;
  public static method Run(): int
    var t := 0;
    for var j in 0 ..< 200 do
      var h := new Holder();
      var f := Program.M<string?>($"fr-{j}");
      h.F := f();
      t := t + (h.F ?? "").Length;
    end for;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine($"{Program.Run()}");
    return 0;
  end Main;
end Program;

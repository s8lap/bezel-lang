//! case: s22_3_8846
//! doc: spec
//! section: 22.3
//! title: a lambda write through a `ref h.f` field alias at `string?` is visible through the field
//! rule: A `ref` argument naming a mutable local or writable field is a true alias: callee writes are visible through the caller's location immediately
//! cites: 14.1, 27.5, 28.9
//! expect: run
//! output: 890
//! notes: Pins #9119 (fix 639eb18fe, review r43-rev-m59-9119, probe x_field_sopt).
//!  `M<string?>(ref h.f, ...)` aliases the field; the lambda writes `q{j}` into it and the caller reads `h.f`: 1 + (1 + digits(j)) per j, sum 890.
//!  On the parent compiler the value the lambda stored was not retained, so the caller's later read is a use-after-free
//!  (ASan: heap-use-after-free in the caller; a plain run diverges or aborts).
module Conf;
public class Holder
  public f: string?;
  public constructor() f := "init"; end constructor;
end Holder;
public class Program
  public static method M<T>(ref p: T, q: T): int
    var w := lambda(): int => do p := q; return 1; end;
    return w();
  end M;
  public static method Run(): int
    var t := 0;
    var h := new Holder();
    for var j in 0..<200 do t := t + Program.M<string?>(ref h.f, $"q{j}"); t := t + (h.f ?? "").Length; end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine($"{Program.Run()}");
    return 0;
  end Main;
end Program;

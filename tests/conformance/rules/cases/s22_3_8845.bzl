//! case: s22_3_8845
//! doc: spec
//! section: 22.3
//! title: a second lambda re-reading a captured `ref p: T` at `string?` after a lambda write stays well-formed
//! rule: A `ref` argument naming a mutable local or writable field is a true alias: callee writes are visible through the caller's location immediately
//! cites: 14.1, 27.5, 28.9
//! expect: run
//! output: 1090
//! notes: Pins #9119 (fix 639eb18fe, review r43-rev-m59-9119, probe x_reread_in_lambda).
//!  The first lambda writes `p := q`, a second lambda reads `p`; M returns 2. Then the caller reassigns `s := "z{j}"` and reads its length: 2 + (1 + digits(j)) per j, sum 400 + 200 + 490 = 1090.
//!  On the parent compiler the value the lambda stored was not retained, so the caller's later read is a use-after-free
//!  (ASan: heap-use-after-free in the caller; a plain run diverges or aborts).
module Conf;
public class Program
  public static method M<T>(ref p: T, q: T): int
    var w := lambda(): int => do p := q; return 1; end;
    var a := w();
    var r := lambda(): int => do var x := p; return 1; end;
    return a + r();
  end M;
  public static method Run(): int
    var t := 0;
    for var j in 0..<200 do
      var s: string? := $"s{j}";
      t := t + Program.M<string?>(ref s, $"q{j}");
      s := $"z{j}";
      t := t + (s ?? "").Length;
    end;
    return t;
  end Run;
  public static method Main(): int
    Console.WriteLine($"{Program.Run()}");
    return 0;
  end Main;
end Program;

//! case: s22_3_8847
//! doc: spec
//! section: 22.3
//! title: an iterator body: a lambda write through a captured generic `ref p: T` at `string?` is visible
//! rule: A `ref` argument naming a mutable local or writable field is a true alias: callee writes are visible through the caller's location immediately
//! cites: 14.1, 27.5, 28.9
//! expect: run
//! output: 890
//! notes: Pins #9119 (fix 639eb18fe, review r43-rev-m59-9119, probe m_sopt_interp_once_1_iter).
//!  The same program as 8844 inside an iterator (`yield return t`): 890.
//!  On the parent compiler the value the lambda stored was not retained, so the caller's later read is a use-after-free
//!  (ASan: heap-use-after-free in the caller; a plain run diverges or aborts).
module Conf;
public class Program
  public static readonly F: string := "fld";
  public static method Mk(j: int): string return $"c{j}"; end Mk;
  public static method M<T>(ref p: T, q: T): int
    var w := lambda(): int => do p := q; return 1; end;
    var a := 0;
    a := a + w();
    return a;
  end M;
  public static method Gen(): IEnumerable<int>
    var t := 0;
    for var j in 0..<200 do
      var loc: string? := $"L{j}";
      var s: string? := $"s{j}";
      t := t + Program.M<string?>(ref s, $"q{j}");
      t := t + (s ?? "").Length;
    end;
    yield return t;
  end Gen;
  public static method Run(): int
    var tot := 0;
    for var v in Program.Gen() do tot := tot + v; end;
    return tot;
  end Run;
  public static method Main(): int
    Console.WriteLine($"{Program.Run()}");
    return 0;
  end Main;
end Program;

//! case: l2_2_0037
//! doc: stdlib
//! section: 2.2
//! title: Math.Abs of a byte? narrowed non-null is ambiguous and refused
//! rule: stdlib §2.2 declares only Abs(int), Abs(long), Abs(double), Abs(decimal). §5.1 admits a `byte` at more than one of them and it matches none exactly, so the candidates are equally ranked (§22.6: "two candidates that both require an implicit conversion at the same position are equally ranked there") and the call is ambiguous, a compile error. Narrowing a `byte?` (§7.4) yields a plain `byte`, which does not change that.
//! cites: 2.2, 5.1, 7.4, 22.6
//! expect: reject
//! diagnostic: ambiguous
module Conf;

public class Program
  public static method IdB(x: byte?): byte? return x; end IdB;
  public static method Main()
    var b: byte? := IdB(byte(200));
    if b <> null then Console.WriteLine(Math.Abs(b)); end;
  end Main;
end Program;

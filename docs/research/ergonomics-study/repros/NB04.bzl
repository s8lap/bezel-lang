// bug: NB4
// title: a collection-literal local of record or choice elements is classified Borrowed
// issue: #8498
// rests-on: spec §15.4 (v0.359) and the §28.3 atoms: a reference-typed collection literal is a fresh owned production
// source: r42/data/new-compiler-bugs.md, verbatim below this header
// expect: run
// stdout: 2
module P;
public record R(V: int);
public choice Sh
  C(r: double);
end Sh;
public class Holder
  private owned _xs: Array<R>;
  public constructor(owned xs: Array<R>)
    _xs := xs;
  end constructor;
  public method Count(): int => _xs.Count;
end Holder;
public class Program
  public static method Main()
    var xs: Array<R> := [new R(1), new R(2)];
    using var h := new Holder(move xs);
    Console.WriteLine(h.Count());
  end Main;
end Program;

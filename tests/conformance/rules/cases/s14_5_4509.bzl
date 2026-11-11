//! case: s14_5_4509
//! doc: spec
//! section: 14.5
//! title: a lambda whose result is a record binds the result type parameter to the
//!   record
//! rule: the lambda's inferred return type then unifies against the function type's
//!   return position, binding type parameters still free
//! cites: 22.5
//! expect: run
//! output: 3
//!   6
//! issue: 8394
module Conf;

public record Pt(X: int, Y: int);

public static class Seq
  public static method Sel<T, R>(xs: Array<T>, f: (T) -> R): owned Array<R>
    var r := new Array<R>();
    for var x in xs do r.Add(f(x)); end;
    return r;
  end Sel;
end Seq;

public class Program
  public static method Main()
    var xs := new Array<int>(); xs.Add(1); xs.Add(2);
    var r := Seq.Sel(xs, lambda(v) => new Pt(v, v * 2));
    for var e in r do Console.WriteLine(e.X + e.Y); end;
  end Main;
end Program;

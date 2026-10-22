//! case: l9_8_0004
//! doc: stdlib
//! section: 9.8
//! title: Range.Contains is a bool in conditions, boolean composition, comparisons, ternaries and lambdas
//! rule: Stdlib §9.8 (v0.340): "`Contains(v)` is `true` iff the range is nonempty under iteration and `v` lies within
//!   the endpoint extent (exclusive upper bound excluded) ... `Step` affects membership only through
//!   direction/emptiness." The result is a `bool`, so it composes with `not`, `or`, `and`, `= true`, an
//!   if-expression and a lambda body.
//! cites: 16.3
//! expect: run
//! output: if
//!   or
//!   andnot
//!   True
//!   False
//!   True
//!   False
//!   True
//!   1
module Conf;

public class Program
  public static method Main()
    var q := 1..5;
    if q.Contains(3) then Console.WriteLine("if"); end if;
    if q.Contains(9) or q.Contains(1) then Console.WriteLine("or"); end if;
    if q.Contains(2) and not q.Contains(9) then Console.WriteLine("andnot"); end if;
    var b := q.Contains(4);
    Console.WriteLine(b);
    var nb := not q.Contains(4);
    Console.WriteLine(nb);
    var eq := q.Contains(2) = true;
    Console.WriteLine(eq);
    var f := lambda(x: int) => not q.Contains(x);
    Console.WriteLine(f(2));
    Console.WriteLine(f(8));
    var h := lambda(x: int) => (if q.Contains(x) then 1 else 0);
    Console.WriteLine(h(2));
  end Main;
end Program;

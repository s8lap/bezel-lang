//! case: s17_4_0909
//! doc: spec
//! section: 17.4
//! title: a forwarded as-IBar on a boxed R that does not implement IBar yields null
//! rule: A successful cast yields a non-null borrow narrowable to the target type; a failed cast yields
//!   `null`.
//! cites: 8.6, 12.7
//! expect: run
//! output: 0
//! issue: 8011
module Conf;

public class Sink
  public saved: Object? := null;
end Sink;

public interface IFoo
  method Get(): int;
end IFoo;

public record R(X: int): IFoo
  public method Get(): int => X;
end R;

public interface IBar
  method Other(): int;
end IBar;

public class Program
  public static method Inner(o: Object): int => (if (o as IBar) <> null then 1 else 0);

  public static method Tag(o: Object): int => Inner(o);

  public static method Main()
    var i := 0;
    var n := 0;
    while i < 100 do
      n := n + Tag(new R(i + 1000));
      i := i + 1;
    end while;
    Console.WriteLine(n);
  end Main;
end Program;

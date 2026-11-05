//! case: s12_7_0901
//! doc: spec
//! section: 12.7
//! title: a forwarded is-IFoo test on a boxed record that implements IFoo is true
//! rule: `is T` tests whether a value is an instance of `T` and produces `bool`.
//! cites: 8.6, 17.4
//! expect: run
//! output: 100
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

public class Program
  public static method Inner(o: Object): int => (if o is IFoo then 1 else 0);

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

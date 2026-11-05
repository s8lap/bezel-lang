//! case: s17_4_0903
//! doc: spec
//! section: 17.4
//! title: an as-IFoo view of a boxed record returned through two forwarders and stored still reads back
//! rule: A successful cast yields a non-null borrow narrowable to the target type; a failed cast yields
//!   `null`.
//! cites: 8.6, 28.9
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
  public static method Id(o: Object): Object? => o as IFoo;

  public static method Fwd(o: Object): Object? => Id(o);

  public static method Tag(s: Sink, o: Object)
    s.saved := Fwd(o);
  end Tag;

  public static method Main()
    using var s := new Sink();
    var i := 0;
    var n := 0;
    while i < 100 do
      Tag(s, new R(i + 1000));
      var g := s.saved as IFoo;
      if g <> null and g.Get() = i + 1000 then
        n := n + 1;
      end if;
      i := i + 1;
    end while;
    Console.WriteLine(n);
  end Main;
end Program;

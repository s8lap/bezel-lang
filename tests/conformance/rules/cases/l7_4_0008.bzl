//! case: l7_4_0008
//! doc: stdlib
//! section: 7.4
//! title: An un-narrowed nullable record passed to TextWriter.WriteLine is rejected
//! rule: public method WriteLine(value: Object);
//! cites: 7.4, 7.3, 5.1
//! expect: reject
//! diagnostic: nullable but the target type is non-nullable
//! issue: 7965
//! notes: twin of l7_4_0003 (pairs the rejected with its accepting form).
module Conf;

public record P(X: int, Y: int);

public class Program
  public static method Show(p: P?)
    var t: TextWriter := Console.Out;
    t.WriteLine(p);
  end Show;

  public static method Main()
    Show(new P(1, 2));
  end Main;
end Program;

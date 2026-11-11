//! case: l2_6_4303
//! doc: stdlib
//! section: 2.6
//! title: a Guid passed at an IEquatable<Guid> parameter equals itself and not another value
//! rule: public record Guid: IComparable<Guid>, IEquatable<Guid>
//! cites: 2.6, 2.9, 5.1
//! expect: run
//! output: True
//!   False
//! issue: 8288
module Conf;

public class Program
  static method Same(x: IEquatable<Guid>, y: Guid): bool
    return x.Equals(y);
  end Same;

  public static method Main()
    var hi := Guid.Parse("22222222-2222-2222-2222-222222222222");
    Console.WriteLine(Same(hi, Guid.Parse("22222222-2222-2222-2222-222222222222")));
    Console.WriteLine(Same(hi, Guid.Empty));
  end Main;
end Program;

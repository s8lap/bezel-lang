//! case: l5_0061
//! doc: stdlib
//! section: 5
//! title: Min and Max with an identity Guid selector keep a single or a repeated element whole
//! rule: public method Min<U>(selector: (T) -> U): U where U: IComparable<U>;
//! cites: 2.6, 5
//! expect: run
//! output: 0a0b0c0d-0e0f-1011-1213-141516171819
//!   0a0b0c0d-0e0f-1011-1213-141516171819
//!   0a0b0c0d-0e0f-1011-1213-141516171819
//! issue: 8048
//! notes: twin of l5_0060 (pairs the accepted with its rejecting form).
module Conf;

public class Program
  public static method Main()
    using var one := new Array<Guid>();
    one.Add(Guid.Parse("0a0b0c0d-0e0f-1011-1213-141516171819"));
    Console.WriteLine(one.Min(lambda(x: Guid) => x).ToString());
    Console.WriteLine(one.Max(lambda(x: Guid) => x).ToString());
    using var two := new Array<Guid>();
    two.Add(Guid.Parse("0a0b0c0d-0e0f-1011-1213-141516171819"));
    two.Add(Guid.Parse("0a0b0c0d-0e0f-1011-1213-141516171819"));
    Console.WriteLine(two.Min(lambda(x: Guid) => x).ToString());
  end Main;
end Program;

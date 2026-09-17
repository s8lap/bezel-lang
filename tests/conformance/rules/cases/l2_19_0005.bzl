//! case: l2_19_0005
//! doc: stdlib
//! section: 2.19
//! title: a user class implementing ICollectionLiteralTarget is a literal
//!   target
//! rule: A user class declares exactly one of ICollectionLiteralTarget<TSelf,
//!   T> / IOwningCollectionLiteralTarget<TSelf, T>, at TSelf = itself, and
//!   supplies FromCollectionLiteral — the sequence-literal target.
//! cites: 2.19, 15.4.1
//! expect: run
//! stdout: 3
module Conf;

public class Ring<T>: ICollectionLiteralTarget<Ring<T>, T>
  private readonly _count: int;

  private constructor(count: int)
    _count := count;
  end constructor;

  public static method FromCollectionLiteral(elements: ReadOnlyArraySpan<T>): owned Ring<T> =>
    new Ring<T>(elements.Length);

  public property Count: int => _count;
end Ring;

public class Program
  public static method Main()
    using var r: Ring<int> := [1, 2, 3];
    Console.WriteLine(r.Count);
  end Main;
end Program;

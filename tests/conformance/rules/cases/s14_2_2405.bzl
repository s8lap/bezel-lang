//! case: s14_2_2405
//! doc: spec
//! section: 14.2
//! title: A swapped type-parameter order reaching IComparable<int> does not satisfy
//!   T: IComparable<T>
//! rule: `where T: IFoo` admits exactly the substitutions for which T conforms to
//!   `IFoo` per the rule above.
//! cites: 14.2, 26.5
//! expect: reject
//! diagnostic: IComparable
//! notes: Single ground: IOrd<int, IShape> extends IComparable<int>, so IShape does
//!   not conform to IComparable<IShape>. The only edit from the previous case's
//!   chain is the type parameter in the IComparable clause. Deleting the Min()
//!   call leaves a compiling file. Not red on base: the pre-fix compiler also
//!   rejected it.
module Conf;

public interface IOrd<A, B>: IComparable<A>
end IOrd;

public interface IShape: IOrd<int, IShape>
  method Area(): int;
end IShape;

public class Program
  public static method Main(): int
    using var a := new Array<IShape>();
    Console.WriteLine(a.Min().Area());
    return 0;
  end Main;
end Program;

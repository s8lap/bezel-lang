//! case: s14_2_2411
//! doc: spec
//! section: 14.2
//! title: A chain through IOrd<int> does not satisfy T: IComparable<T> at IShape
//! rule: `where T: IFoo` admits exactly the substitutions for which T conforms to
//!   `IFoo` per the rule above.
//! cites: 14.2, 26.5
//! expect: reject
//! diagnostic: IComparable
//! notes: Single ground: IOrd<int> extends IComparable<int>, not
//!   IComparable<IShape>. The accepting twin is the IOrd<IShape> chain of
//!   s14_2_2401. Deleting the Min() call leaves a compiling file. Not red on
//!   base: the pre-fix compiler also rejected it.
module Conf;

public interface IOrd<T>: IComparable<T>
end IOrd;

public interface IShape: IOrd<int>
  method Area(): int;
end IShape;

public class Program
  public static method Main(): int
    using var a := new Array<IShape>();
    Console.WriteLine(a.Min().Area());
    return 0;
  end Main;
end Program;

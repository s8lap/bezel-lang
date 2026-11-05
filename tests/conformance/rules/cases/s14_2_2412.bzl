//! case: s14_2_2412
//! doc: spec
//! section: 14.2
//! title: A chain through IOrd<IOther> does not satisfy T: IComparable<T> at IShape
//! rule: `where T: IFoo` admits exactly the substitutions for which T conforms to
//!   `IFoo` per the rule above.
//! cites: 14.2, 26.5
//! expect: reject
//! diagnostic: IComparable
//! notes: Single ground: IShape reaches IComparable<IOther>, a different
//!   instantiation from IComparable<IShape>. Deleting the Min() call leaves a
//!   compiling file. Not red on base: the pre-fix compiler also rejected it.
module Conf;

public interface IOrd<T>: IComparable<T>
end IOrd;

public interface IOther
  method X(): int;
end IOther;

public interface IShape: IOrd<IOther>
  method Area(): int;
end IShape;

public class Program
  public static method Main(): int
    using var a := new Array<IShape>();
    Console.WriteLine(a.Min().Area());
    return 0;
  end Main;
end Program;

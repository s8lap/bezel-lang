//! case: s14_2_2410
//! doc: spec
//! section: 14.2
//! title: An interface reaching only IComparable<int> does not satisfy T:
//!   IComparable<T>
//! rule: `where T: IFoo` admits exactly the substitutions for which T conforms to
//!   `IFoo` per the rule above.
//! cites: 14.2, 26.5
//! expect: reject
//! diagnostic: IComparable
//! notes: Single ground: IShape reaches IComparable<int> only, never
//!   IComparable<IShape>. Deleting the Min() call leaves a compiling file. Not
//!   red on base: the pre-fix compiler also rejected it.
module Conf;

public interface IOrd: IComparable<int>
end IOrd;

public interface IShape: IOrd
  method Area(): int;
end IShape;

public class Program
  public static method Main(): int
    using var a := new Array<IShape>();
    Console.WriteLine(a.Min().Area());
    return 0;
  end Main;
end Program;

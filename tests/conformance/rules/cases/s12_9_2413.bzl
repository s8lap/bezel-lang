//! case: s12_9_2413
//! doc: spec
//! section: 12.9
//! title: IComparable is invariant: IDerived, whose base reaches IComparable<IBase>,
//!   fails T: IComparable<T>
//! rule: Type parameters without `out` or `in` are invariant.
//! cites: 14.2, 2.8
//! expect: reject
//! diagnostic: IComparable
//! notes: Single ground: IDerived reaches IComparable<IBase>, and a type parameter
//!   without out or in is invariant, so IDerived does not conform to
//!   IComparable<IDerived>. Stdlib 2.8 states that IComparable is invariant.
//!   Deleting the Min() call leaves a compiling file. Not red on base: the
//!   pre-fix compiler also rejected it.
module Conf;

public interface IOrd<T>: IComparable<T>
end IOrd;

public interface IBase: IOrd<IBase>
  method Area(): int;
end IBase;

public interface IDerived: IBase
  method Z(): int;
end IDerived;

public class Program
  public static method Main(): int
    using var a := new Array<IDerived>();
    Console.WriteLine(a.Min().Area());
    return 0;
  end Main;
end Program;

//! case: s14_2_2414
//! doc: spec
//! section: 14.2
//! title: A generic interface extending IComparable<IG<int>> fails T: IComparable<T>
//!   at IG<string>
//! rule: `where T: IFoo` admits exactly the substitutions for which T conforms to
//!   `IFoo` per the rule above.
//! cites: 14.2, 26.5
//! expect: reject
//! diagnostic: IComparable
//! notes: Single ground: IG<string> reaches IComparable<IG<int>>, not
//!   IComparable<IG<string>>; at IG<int> the same declaration would conform.
//!   The Area() member is only there so the call chain a.Min().Area() is
//!   well-formed if Min() were admitted. Deleting the Min() call leaves a
//!   compiling file. Not red on base: the pre-fix compiler also rejected it.
module Conf;

public interface IG<T>: IComparable<IG<int>>
  method Area(): int;
end IG;

public class Program
  public static method Main(): int
    using var a := new Array<IG<string>>();
    Console.WriteLine(a.Min().Area());
    return 0;
  end Main;
end Program;

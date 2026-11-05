//! case: s14_2_1901
//! doc: spec
//! section: 14.2
//! title: Array<Guid?>.Sort is rejected: a type bound admits only non-nullable substitutions
//! rule: A type-bound constraint — naming a class or interface — implies non-null T.
//! cites: 4.4, 7.1
//! expect: reject
//! diagnostic: IComparable
module Conf;

class Program
  public static method Main()
    var a: Array<Guid?> := [Guid.Parse("22222222-2222-2222-2222-222222222222"), null];
    a.Sort();
  end Main;
end Program;

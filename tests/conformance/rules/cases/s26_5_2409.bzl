//! case: s26_5_2409
//! doc: spec
//! section: 26.5
//! title: Array<Array<int>>.Min() with no extension in scope names no member
//! rule: Where neither rule 1 nor this search supplies the name, nothing supplies
//!   it: the access names no member and is a compile error at the name
//! cites: 26.5, 14.2, 26.5.1
//! expect: reject
//! diagnostic: IComparable
//! notes: The accepting twin is the first case of this group (s26_5_2401): the only
//!   edit is the user extension on Object. Array<int> is not
//!   IComparable<Array<int>>, so the stdlib block is filtered out and nothing
//!   supplies Min. Deleting the Min() line leaves a compiling file. Not red on
//!   base: the pre-fix compiler also rejected it.
module Conf;

public class Program
  public static method Main(): int
    using var a := new Array<Array<int>>();
    Console.WriteLine(a.Min());
    return 0;
  end Main;
end Program;

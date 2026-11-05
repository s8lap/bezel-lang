//! case: l4_4_1909
//! doc: stdlib
//! section: 4.4
//! title: Array<DictionaryEntry<int, int>>.Sort is rejected: no IComparable conformance
//! rule: public method Sort() where T: IComparable<T>;
//! cites: 14.2, 4
//! expect: reject
//! diagnostic: IComparable
module Conf;

class Program
  public static method Main()
    var a: Array<DictionaryEntry<int, int>> := [];
    a.Sort();
  end Main;
end Program;

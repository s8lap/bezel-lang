//! case: l4_4_1908
//! doc: stdlib
//! section: 4.4
//! title: Array<Range<int>>.Sort is rejected: Range<T> is only IEnumerable<T>
//! rule: public method Sort() where T: IComparable<T>;
//! cites: 14.2
//! expect: reject
//! diagnostic: IComparable
module Conf;

class Program
  public static method Main()
    var a: Array<Range<int>> := [];
    a.Sort();
  end Main;
end Program;

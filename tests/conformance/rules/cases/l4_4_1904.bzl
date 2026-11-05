//! case: l4_4_1904
//! doc: stdlib
//! section: 4.4
//! title: Array<Object>.Sort is rejected: Object does not implement IComparable<Object>
//! rule: public method Sort() where T: IComparable<T>;
//! cites: 14.2, 2.8
//! expect: reject
//! diagnostic: IComparable
module Conf;

class Program
  public static method Main()
    var a: Array<Object> := [Guid.Parse("22222222-2222-2222-2222-222222222222")];
    a.Sort();
  end Main;
end Program;

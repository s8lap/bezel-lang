//! case: l4_4_1905
//! doc: stdlib
//! section: 4.4
//! title: Array<bool>.Sort is rejected: Boolean conforms to IEquatable only
//! rule: public method Sort() where T: IComparable<T>;
//! cites: 14.2, 2.8
//! expect: reject
//! diagnostic: IComparable
module Conf;

class Program
  public static method Main()
    var a: Array<bool> := [true, false];
    a.Sort();
  end Main;
end Program;

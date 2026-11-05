//! case: l4_4_1906
//! doc: stdlib
//! section: 4.4
//! title: Array<IntPtr>.Sort is rejected: IntPtr declares no IComparable conformance
//! rule: public method Sort() where T: IComparable<T>;
//! cites: 14.2, 10
//! expect: reject
//! diagnostic: IComparable
module Conf;

class Program
  public static method Main()
    var a: Array<IntPtr> := [];
    a.Sort();
  end Main;
end Program;

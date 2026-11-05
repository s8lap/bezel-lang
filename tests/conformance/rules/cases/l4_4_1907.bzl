//! case: l4_4_1907
//! doc: stdlib
//! section: 4.4
//! title: Array<CancellationToken>.Sort is rejected: CancellationToken declares no IComparable conformance
//! rule: public method Sort() where T: IComparable<T>;
//! cites: 14.2, 6.1
//! expect: reject
//! diagnostic: IComparable
module Conf;

class Program
  public static method Main()
    var a: Array<CancellationToken> := [];
    a.Sort();
  end Main;
end Program;

//! case: l4_1804
//! doc: stdlib
//! section: 4
//! title: IList<R>.Contains is refused at a record with no equality of its own
//! rule: It has none exactly where the same comparison between two values of the
//!   type draws §9.2's, §35.7's or §15.1.2's use-site error, as for a record or
//!   choice that is neither auto-hashable nor declares an `Equals` of its own
//! cites: 4.1, 9.2
//! expect: reject
//! diagnostic: first kind
//! issue: 7598
module Conf;

public record RX(fn: (int) -> int) end RX;
class Program
  public static method Main()
    var f: (int) -> int := lambda(x: int) => x + 1;
    var arr := new Array<RX>();
    var lst: IList<RX> := arr;
    Console.WriteLine(lst.Contains(new RX(f)));
  end Main;
end Program;

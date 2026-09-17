//! case: l4_15_0001
//! doc: stdlib
//! section: 4.15
//! title: move is not admitted through an ArraySpan<T> indexer for any T
//! rule: move arrSpan[i] is not admitted for any T -- borrowed slots have no
//!   ownership to transfer; the plain indexer-get is the only read form.
//! cites: 4.15
//! expect: reject
//! diagnostic: move
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    using var arr: Array<Widget> := [new Widget()];
    var span := arr.AsSpan();
    var w := move span[0];
  end Main;
end Program;

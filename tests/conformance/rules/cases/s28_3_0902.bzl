//! case: s28_3_0902
//! doc: spec
//! section: 28.3
//! title: an instance method returning a receiver-owned Array field through an unmarked return is admitted
//! rule: return only a receiver-(parameter-)sourced borrow
//! cites: 26.7
//! expect: run
//! output: 5
module Conf;

public class Holder
  private owned _items: Array<int>;

  public constructor()
    _items := new Array<int>();
    _items.Add(5);
  end constructor;

  public method Items(): Array<int> => _items;
end Holder;

public class Program
  public static method Main()
    using var h := new Holder();
    Console.WriteLine(h.Items()[0]);
  end Main;
end Program;

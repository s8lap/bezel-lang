//! case: s19_2_0002
//! doc: spec
//! section: 19.2
//! title: an arm result may not mark the scrutinee's own binding
//! rule: An arm result marking the scrutinee's own binding is always
//!   rejected by the consume-site freeze — the scrutinee borrow is live
//!   through the selected arm's completion.
//! cites: 19.2, 28.3
//! expect: reject
//! diagnostic: scrutinee
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Pick(): owned Widget
    var w := new Widget();
    var chosen := (case w of
      var x: Widget => move w,
      _             => move w);
    return chosen;
  end Pick;

  public static method Main()
    var w := Pick();
    _ := w;
  end Main;
end Program;

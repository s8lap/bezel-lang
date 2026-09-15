//! case: l4_4_0011
//! doc: stdlib
//! section: 4.4
//! title: a fresh producer stored via Array<T>.Add warns rather than rejects
//! rule: On the borrowing family a fresh producer via Add/Insert/indexer-set
//!   is admitted but warns: the temporary dies at the statement's end and
//!   the stored slot dangles.
//! cites: 4.4, 15.4, 28.11
//! expect: warn
//! diagnostic: dangl
module Conf;

public class Widget
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("widget disposed");
  end destructor;
end Widget;

public class Program
  public method Fill()
    using var arr := new Array<Widget>();
    arr.Add(new Widget());
  end Fill;
end Program;

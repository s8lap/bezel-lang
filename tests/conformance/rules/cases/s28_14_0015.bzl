//! case: s28_14_0015
//! doc: spec
//! section: 28.14
//! title: a fresh producer argument at a scoped class-typed parameter position is rejected
//! rule: A fresh producer argument is rejected — the unbound owned
//!   temporary dies at the end of the full expression; bind the source
//!   first.
//! cites: 28.14
//! expect: reject
//! diagnostic: temporary
module Conf;

public class Stream
  public constructor()
  end constructor;
end Stream;

public class Reader
  private _stream: Stream;

  public constructor(scoped stream: Stream)
    _stream := stream;
  end constructor;
end Reader;

public class Program
  public static method Main()
    var r := new Reader(scoped new Stream());
  end Main;
end Program;

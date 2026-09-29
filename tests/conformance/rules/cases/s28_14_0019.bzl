//! case: s28_14_0019
//! doc: spec
//! section: 28.14
//! title: a plain var holder resolves no admitted bounding object for a scoped borrow argument
//! rule: A class-typed `scoped` argument's borrow source is admitted, in place
//!   of a closure's capture set, iff the bounding object resolves to a
//!   `using var`/`await using var` local or to `this`; a bounding object that
//!   is none of the admitted resolutions admits no capture-bearing arguments.
//! cites: 28.14, 22.3
//! expect: reject
//! diagnostic: bounding object
module Conf;

public class Stream
  public constructor()
  end constructor;

  public method Read()
  end Read;
end Stream;

public class Reader
  private _stream: Stream;

  public constructor(scoped stream: Stream)
    _stream := stream;
  end constructor;

  public method Pull()
    _stream.Read();
  end Pull;
end Reader;

public class Program
  public method Demo()
    using var s := new Stream();
    var r := new Reader(scoped s);
    r.Pull();
  end Demo;
end Program;

//! case: s28_14_0014
//! doc: spec
//! section: 28.14
//! title: a scoped class-typed parameter may be stored in a direct non-owned field of the bounding object
//! rule: On a class- or interface-typed parameter the callee may store the
//!   borrow in a direct non-owned field of the bounding object; the call
//!   site writes M(scoped x), an argument modifier, and the argument's
//!   borrow source must satisfy the declared-before test against the
//!   bounding `using var` local the constructor call initializes.
//! cites: 28.14
//! expect: run
//! stdout: read
module Conf;

public class Stream
  public constructor()
  end constructor;

  public method Read()
    Console.WriteLine("read");
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
  public static method Main()
    using var s := new Stream();
    using var r := new Reader(scoped s);
    r.Pull();
  end Main;
end Program;

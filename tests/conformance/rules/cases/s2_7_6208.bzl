//! case: s2_7_6208
//! doc: spec
//! section: 2.7
//! title: a local variable named FileStream is an ordinary receiver; its
//!   Read overload is the user's
//! rule: local scope, then the enclosing type's accessible members
//! cites: 2.7, 22.6
//! expect: run
//! stdout: 4
module Conf;

public class Source
  public constructor() end constructor;

  public method Read(b: ReadOnlySpan<int>): int
    return b.Length;
  end Read;
end Source;

public class Program
  public static method Main()
    using var ints := new OwningBuffer<int>(4);
    var FileStream := new Source();
    Console.WriteLine(FileStream.Read(ints.AsReadOnlySpan()));
  end Main;
end Program;

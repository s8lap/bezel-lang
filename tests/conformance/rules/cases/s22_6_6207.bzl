//! case: s22_6_6207
//! doc: spec
//! section: 22.6
//! title: a user class's own Read/Write overloads over spans resolve by
//!   parameter type
//! rule: Methods and constructors may be overloaded by parameter list.
//! cites: 22.6
//! expect: run
//! stdout: 4 104 3
module Conf;

public class MyStream
  public constructor() end constructor;

  public method Read(b: ReadOnlySpan<int>): int
    return b.Length;
  end Read;

  public method Read(b: ReadOnlySpan<byte>): int
    return 3;
  end Read;

  public method Write(b: Span<int>): int
    return b.Length + 100;
  end Write;
end MyStream;

public class Program
  public static method Main()
    using var ints := new OwningBuffer<int>(4);
    using var bytes := new OwningBuffer<byte>(4);
    var s := new MyStream();
    Console.WriteLine($"{s.Read(ints.AsReadOnlySpan())} {s.Write(ints.AsSpan())} {s.Read(bytes.AsReadOnlySpan())}");
  end Main;
end Program;

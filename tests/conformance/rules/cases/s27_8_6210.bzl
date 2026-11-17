//! case: s27_8_6210
//! doc: spec
//! section: 27.8
//! title: an alias of ReadOnlySpan<byte> is accepted at FileStream.Write
//! rule: Type aliases — generic or not — see through to the aliased type for
//!   identity, equality, method dispatch, and overload resolution.
//! cites: 27.8, 7.1
//! expect: run
//! stdout: 3 7 8 9
module Conf;

import System.IO;

type Bytes := ReadOnlySpan<byte>;

public class Program
  static method Produce(path: string)
    using var src := new OwningBuffer<byte>(3, lambda(i) => byte(i + 7));
    var s: Bytes := src.AsReadOnlySpan();
    using var w := new FileStream(path, FileMode.Create, FileAccess.Write);
    w.Write(s);
  end Produce;

  static method ReadBack(path: string): string
    using var dst := new OwningBuffer<byte>(8);
    using var r := new FileStream(path, FileMode.Open, FileAccess.Read);
    var total := 0;
    var n := r.Read(dst.AsSpan());
    while n > 0 do
      total := total + n;
      n := r.Read(dst.AsSpan(total));
    end;
    return $"{total} {dst[0]} {dst[1]} {dst[2]}";
  end ReadBack;

  public static method Main()
    var path := "s27_8_6210.tmp";
    Produce(path);
    Console.WriteLine(ReadBack(path));
    File.Delete(path);
  end Main;
end Program;

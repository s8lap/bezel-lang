//! case: s32_5_0018
//! doc: spec
//! section: 32.5
//! title: a temporary inline-array argument at an extern call site is rejected
//! rule: A temporary or otherwise non-assignable inline-array argument is a compile
//!   error.
//! cites: 32.5, 15.1.3
//! expect: reject
//! diagnostic: assignable
module Conf;

extern "libexample"
  method Fill(buf: [4]int): ();
end;

public static class Buffers
  public static method MakeBuffer(): [4]int
    var buf: [4]int;
    return buf;
  end MakeBuffer;

  public static method Bad()
    Fill(MakeBuffer());
  end Bad;
end Buffers;

//! case: l7_1_0001
//! doc: stdlib
//! section: 7.1
//! title: Stream has no async Span<byte> overload — only the windowed Array<byte> async form exists
//! rule: No async span overloads (v0.179, #1438). Asynchronous byte I/O
//!   uses the heap-owned windowed form — ReadAsync/WriteAsync(buffer:
//!   Array<byte>, offset, count, token); the async span overloads were
//!   removed.
//! cites: 7.1
//! expect: reject
//! diagnostic: ReadAsync
module Conf;

import System.IO;

public class Program
  public method Attempt(fs: FileStream, buf: Span<byte>)
    using var t := fs.ReadAsync(buf, 0, 16);
  end Attempt;
end Program;

//! case: s32_5_0018_call
//! doc: spec
//! section: 32.5
//! title: a non-assignable inline-array argument at an extern call site is rejected
//! rule: A temporary or otherwise non-assignable inline-array argument is a compile
//!   error (§32.5 line 9706, v0.309). The by-address marshalling requires the address
//!   of the CALLER's storage cross the boundary, so the argument must be one of the
//!   §22.3 location categories — an Identifier, a MemberAccessExpr hop, or an IndexExpr
//!   hop, with postfix `!` and chain hops stripped.
//! cites: 32.5, 22.3
//! expect: reject
//! diagnostic: assignable
//!
//! Regression pin for fix #4660 commit 5 (s32_5_0018). Pre-fix, the compiler
//! silently accepted `Fill(MakeBuffer())` — `MakeBuffer()` returns an inline-array
//! value with no storage location to take the address of, so the by-address
//! marshalling would have crossed an invalid address. Post-fix, the call-site
//! check `checkCallSiteInlineArrayArg` rejects this with a `non-assignable`
//! diagnostic citing §32.5 / §22.3.

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

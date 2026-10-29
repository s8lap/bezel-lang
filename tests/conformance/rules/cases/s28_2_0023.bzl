//! case: s28_2_0023
//! doc: spec
//! section: 28.2
//! title: a nested class implementing IDisposable without declaring Dispose is accepted
//! rule: "The compiler generates `IDisposable` on every non-static class." The
//!   class satisfies IDisposable through the compiler-generated `Dispose()`, so
//!   a nested class that names IDisposable and declares no `Dispose` member is
//!   an ordinary class. This is the accepting companion to the rejected
//!   declarations of s28_2_0018 to s28_2_0021.
//! cites: 28.2
//! expect: accept
module Conf;

public class Outer
  public class C: IDisposable
    private _x: int := 1;
  end C;
end Outer;

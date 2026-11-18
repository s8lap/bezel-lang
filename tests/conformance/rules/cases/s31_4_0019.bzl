//! case: s31_4_0019
//! doc: spec
//! section: 31.4
//! title: a class under OperationCanceledException may not list IFailure
//! rule: And a class with `OperationCanceledException` in its base chain may
//!   not implement it: listing `IFailure`, or an interface extending it, on
//!   such a class is a compile error at the declaration, so cancellation is
//!   cancellation in every subclass.
//! cites: 31.4
//! expect: reject
//! diagnostic: base chain
//! issue: 8416
module Conf;

public class Shutdown: OperationCanceledException, IFailure
end Shutdown;

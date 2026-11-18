//! case: s31_4_0020
//! doc: spec
//! section: 31.4
//! title: a class under OperationCanceledException without the marker is admitted
//! rule: And a class with `OperationCanceledException` in its base chain may
//!   not implement it: listing `IFailure`, or an interface extending it, on
//!   such a class is a compile error at the declaration, so cancellation is
//!   cancellation in every subclass.
//! cites: 31.4
//! expect: accept
//! issue: 8416
module Conf;

public class Shutdown: OperationCanceledException
end Shutdown;

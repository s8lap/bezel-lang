//! case: s31_4_0028
//! doc: spec
//! section: 31.4
//! title: a class whose user base derives from OperationCanceledException is admitted without the marker
//! rule: And a class with `OperationCanceledException` in its base chain may
//!   not implement it: listing `IFailure`, or an interface extending it, on
//!   such a class is a compile error at the declaration, so cancellation is
//!   cancellation in every subclass.
//! cites: 31.4
//! expect: accept
//! issue: 8416
module Conf;

public class Halt(message: string): OperationCanceledException(message)
end Halt;

public class RetryableHalt(message: string): Halt(message)
end RetryableHalt;

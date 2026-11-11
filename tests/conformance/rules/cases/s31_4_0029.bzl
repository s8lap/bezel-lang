//! case: s31_4_0029
//! doc: spec
//! section: 31.4
//! title: a class under OperationCanceledException may not implement an interface extending IFailure
//! rule: And a class with `OperationCanceledException` in its base chain may
//!   not implement it: listing `IFailure`, or an interface extending it, on
//!   such a class is a compile error at the declaration, so cancellation is
//!   cancellation in every subclass. An interface may extend `IFailure`; both
//!   restrictions then bind its implementers.
//! cites: 31.4
//! expect: reject
//! diagnostic: base chain
//! issue: 8416
module Conf;

public interface IRetryable: IFailure
end IRetryable;

public class Shutdown(message: string): OperationCanceledException(message), IRetryable
end Shutdown;

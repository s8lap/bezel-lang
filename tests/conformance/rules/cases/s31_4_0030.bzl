//! case: s31_4_0030
//! doc: spec
//! section: 31.4
//! title: a class under InvalidOperationException may implement an interface extending IFailure
//! rule: Every other `Exception`-derived class may list it, whatever its base:
//!   a class deriving from `InvalidOperationException`, from `FormatException`
//!   or from a class of the program's own is a failure where its author says
//!   so, and an arm naming its base still takes it. An interface may extend
//!   `IFailure`; both restrictions then bind its implementers.
//! cites: 31.4
//! expect: accept
//! issue: 8416
module Conf;

public interface IRetryable: IFailure
end IRetryable;

public class Shutdown(message: string): InvalidOperationException(message), IRetryable
end Shutdown;

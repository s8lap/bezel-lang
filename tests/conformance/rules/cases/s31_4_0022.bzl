//! case: s31_4_0022
//! doc: spec
//! section: 31.4
//! title: a class under InvalidOperationException without the marker is admitted
//! rule: a class that does not inherit `IFailure` may list it only where no class
//!   in its base chain, other than `Exception` itself, is a built-in or
//!   standard-library exception class; listing it otherwise is a compile error
//!   at the declaration. So a class deriving from `OperationCanceledException`
//!   is cancellation in every subclass, and an arm naming
//!   `InvalidOperationException`, or any other defect class of the documents,
//!   never takes a failure that user code declared.
//! cites: 31.4
//! expect: accept
//! issue: 8416
module Conf;

public class OrderError(message: string): InvalidOperationException(message)
end OrderError;

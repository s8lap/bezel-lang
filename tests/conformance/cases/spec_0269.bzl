Exception
├─ ArgumentException                — invalid arguments.
│   └─ ArgumentNullException        — a null argument where non-null was required.
├─ ArithmeticException              — base for arithmetic errors.
│   ├─ OverflowException            — integer or decimal overflow.
│   └─ DivideByZeroException        — division by zero.
├─ InvalidOperationException        — operation not valid in current state.
│   └─ DisposedAccessException      — member access on a disposed instance (checked disposal mode only).
├─ NotImplementedException          — placeholder for unimplemented code.
├─ NullReferenceException           — null encountered where non-null required.
├─ IndexOutOfRangeException         — array or string index out of bounds.
├─ MatchFailureException            — case expression did not match.
├─ OperationCanceledException       — operation cancelled via CancellationToken.
├─ TypeInitializationException      — static initialization failed; wraps the original.
├─ FormatException                  — string does not conform to an expected format.
└─ StackOverflowException           — call stack exhausted; cannot be caught; terminates the program.

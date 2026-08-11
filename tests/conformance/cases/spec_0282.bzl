public class ContractViolationException: Exception
  public pure constructor(message: string);
end ContractViolationException;

public class Exception
  public pure constructor();
  public pure constructor(message: string);
  public constructor(message: string, cause: Exception);   // snapshot chaining, below
  public readonly property Message: string;
  public readonly property StackTrace: string;             // formatted; "" when unavailable
  public readonly property CauseChain: Array<ExceptionInfo>;   // empty when no cause
end Exception;

public record ExceptionInfo(TypeName: string, Message: string, StackTrace: string);

public class ArgumentException: Exception
  public pure constructor(message: string);
  public pure constructor(message: string, paramName: string);
  public readonly property ParamName: string?;   // null when not supplied
end ArgumentException;

Exception
├─ ContractViolationException — a `requires`/`ensures`/`invariant` clause evaluated `false` ([§22.16](#sec-contract-clauses))
├─ ArgumentException                — invalid arguments.
│   └─ ArgumentNullException        — a null argument where non-null was required.
├─ ArithmeticException              — base for arithmetic errors.
│   ├─ OverflowException            — a value outside the destination type's or operation's domain: integer/decimal overflow ([§6.1](#sec-integer-arithmetic)/[§6.4](#sec-decimal-arithmetic)), checked narrowings and enum casts ([§5.2](#sec-explicit-conversions)/[§5.4](#sec-enum-casts)), out-of-domain shift counts ([§6.5](#sec-bitwise-operations)), a zero range step ([§16.4](#sec-stride)).
│   └─ DivideByZeroException        — division by zero.
├─ InvalidOperationException        — operation not valid in current state.
│   └─ DisposedAccessException      — member access on a disposed instance (checked disposal mode only).
├─ NotImplementedException          — placeholder for unimplemented code.
├─ NullReferenceException           — null encountered where non-null required.
├─ IndexOutOfRangeException         — array or string index out of bounds.
├─ MatchFailureException            — case expression did not match.
├─ OperationCanceledException       — operation cancelled via CancellationToken.
├─ TypeInitializationException      — static initialization failed; wraps the original (`InnerException` null for the §22.13 cycle backstop).
├─ FormatException                  — string does not conform to an expected format.
└─ StackOverflowException           — call stack exhausted; cannot be caught; terminates the program.

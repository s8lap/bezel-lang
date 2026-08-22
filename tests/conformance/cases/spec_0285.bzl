public class Exception
  public pure constructor();
  public pure constructor(message: string);
  public constructor(message: string, cause: Exception);   // snapshot chaining, below
  public readonly property Message: string;
  public readonly property StackTrace: string;             // formatted; "" when unavailable
  public readonly property CauseChain: Array<ExceptionInfo>;   // empty when no cause
  public readonly property SupersededChain: Array<ExceptionInfo>;   // v0.259 — empty when nothing superseded
  public override method ToString(): string;               // v0.268 — the rendering, below
end Exception;

public record ExceptionInfo(TypeName: string, Message: string, StackTrace: string);

public class ArgumentException: Exception
  public pure constructor(message: string);
  public pure constructor(message: string, paramName: string);
  public readonly property ParamName: string?;   // null when not supplied
end ArgumentException;

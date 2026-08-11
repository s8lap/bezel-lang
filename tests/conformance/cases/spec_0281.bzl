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

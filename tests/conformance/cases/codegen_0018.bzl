public class Exception
  public pure constructor();   public pure constructor(message: string);
  public constructor(message: string, cause: Exception);     // snapshot chaining; cause NOT retained
  public readonly property Message: string;   StackTrace: string;
  public readonly property CauseChain: Array<ExceptionInfo>;       // outermost cause first
  public readonly property SupersededChain: Array<ExceptionInfo>;  // chronological
  public override method ToString(): string;                       // v0.268; NOT pure
end
public record ExceptionInfo(TypeName: string, Message: string, StackTrace: string);
public class ArgumentException: Exception
  public pure constructor(message: string);
  public pure constructor(message: string, paramName: string);
  public readonly property ParamName: string?;
end

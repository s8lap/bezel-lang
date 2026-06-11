public class TypeInitializationException: Exception
  public readonly property InnerException: Exception?;   // the original initializer exception
end TypeInitializationException;

public class TypeInitializationException: Exception
  public readonly property InnerException: Exception?;   // the original initializer exception (null for the spec §22.13 cycle backstop)
end TypeInitializationException;

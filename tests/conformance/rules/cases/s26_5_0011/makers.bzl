module Factory;
import Lib;

public class WidgetFactory
  public static method Make(size: int): owned Widget => new Widget(size);
end WidgetFactory;

public record PropertyChangedEvent(Sender: Object, PropertyName: string);

public interface INotifyPropertyChanged
  event PropertyChanged: PropertyChangedEvent;
end INotifyPropertyChanged;

public class Person: INotifyPropertyChanged
  public event PropertyChanged: PropertyChangedEvent;

  private _name: string := "";

  public property Name: string
    get => _name;
    set => do
      if _name <> value then
        _name := value;
        PropertyChanged.Raise(new PropertyChangedEvent(this, nameof(Name)));
      end;
    end;
  end Name;
end Person;

var p: INotifyPropertyChanged := new Person();
using var sub := p.PropertyChanged.Subscribe((e) =>
  Console.WriteLine($"Changed: {e.PropertyName}"));

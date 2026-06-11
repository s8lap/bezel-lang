public record PropertyChangedEvent(Sender: Object, PropertyName: string);

public interface INotifyPropertyChanged
  event PropertyChanged: PropertyChangedEvent;
end INotifyPropertyChanged;

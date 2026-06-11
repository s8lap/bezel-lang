var p: INotifyPropertyChanged := new Person();
using var sub := p.PropertyChanged.Subscribe((e) =>
  Console.WriteLine($"Changed: {e.PropertyName}"));

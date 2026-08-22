public class SampleWindow
  private owned _samples: Array<double> := new Array<double>();

  public method Push(value: double)
    _samples.Add(value);
  end Push;

  public method At(index: int): double
    requires(index >= 0)
    requires(index < _samples.Count)

    return _samples[index];
  end At;

  public static pure method Doubled(v: double): double => v * 2.0;
end SampleWindow;

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
end SampleWindow;

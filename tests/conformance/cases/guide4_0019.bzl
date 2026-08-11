public static class App
  private static owned readonly _exiting: SharedEventSource<()> := new();
  public static property Exiting: ISharedEvent<()> => _exiting;
  public static method NotifyExiting() _exiting.Raise(); end NotifyExiting;
end App;

using var sub := App.Exiting.Subscribe(OnExit);   // handler MUST be capture-free:
                                                  //   a static method ref or
                                                  //   non-capturing lambda (§6.5)

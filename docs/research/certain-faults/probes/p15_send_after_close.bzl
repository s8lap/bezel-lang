module Probe;
public class Program
  public static method Main()
    using var ch := new Channel<int>();
    using var tx := ch.NewSender();
    ch.Close();
    tx.Send(5);
    Console.WriteLine("sent");
  end Main;
end Program;

module Query.TopN;

record Player(Name: string, Score: int);

public class Program
  public static method Main()
    var players: Array<Player> := [
      new Player("zed", 50), new Player("amy", 70), new Player("bob", 70),
      new Player("cat", 40), new Player("dan", 90)
    ];
    var top := players.OrderByDescending(lambda(p) => p.Score).ThenBy(lambda(p) => p.Name).Take(3).ToArray();
    var rank := 1;
    for var p in top do
      Console.WriteLine($"{rank}. {p.Name} {p.Score}");
      rank++;
    end;
    Console.WriteLine($"avg {players.Average(lambda(p) => p.Score):F1}");
  end Main;
end Program;

using System;
using System.IO;
using System.Runtime.Serialization.Json;
using System.Text;

public sealed class EffectSettings
{
    public string Effect { get; set; }
    public int ConfettiCount { get; set; }
    public double ConfettiLife { get; set; }
    public double ConfettiSpeed { get; set; }
    public int GravityCount { get; set; }
    public double GravityLife { get; set; }
    public double GravityStrength { get; set; }
    public double CollisionRadius { get; set; }
    public int ClusterCount { get; set; }
    public double ClusterFuse { get; set; }
    public double ClusterSpeed { get; set; }
    public double SlingshotPower { get; set; }
    public bool ClusterGravityEnabled { get; set; }
    public double ClusterGravity { get; set; }
    public int BlackHoleCount { get; set; }
    public int BlackHoleFeedCount { get; set; }
    public double BlackHoleStrength { get; set; }
    public double BlackHoleGrowth { get; set; }
    public double BlackHoleOrbitDamping { get; set; }
    public double BlackHoleOrbitRatio { get; set; }
    public double PendulumLength { get; set; }
    public double PendulumGravity { get; set; }
    public double PendulumDamping { get; set; }
    public bool PendulumElasticEnabled { get; set; }
    public double PendulumStiffness { get; set; }
    public double PendulumMaxStretch { get; set; }
    public double CrossGatherRate { get; set; }
    public double CrossReach { get; set; }
    public int CrossBurstCount { get; set; }
    public double RailChargePerNotch { get; set; }
    public double RailPower { get; set; }
    public int RailImpactCount { get; set; }
    public double DiceSize { get; set; }
    public double DiceSensitivity { get; set; }
    public double DiceStopDelay { get; set; }
    public string FireMode { get; set; }
    public string Palette { get; set; }
    public EffectSettings()
    {
        Effect = "cluster"; ConfettiCount = 280; ConfettiLife = 3.4; ConfettiSpeed = 1;
        GravityCount = 130; GravityLife = 11; GravityStrength = 1; CollisionRadius = 16;
        ClusterCount = 150; ClusterFuse = .9; ClusterSpeed = 1;
        FireMode = "instant"; Palette = "neon"; SlingshotPower = 6; ClusterGravity = 600;
        BlackHoleCount = 180; BlackHoleFeedCount = 15; BlackHoleStrength = 1;
        BlackHoleGrowth = .08; BlackHoleOrbitDamping = .01; BlackHoleOrbitRatio = .8;
        PendulumLength = 180; PendulumGravity = 1000; PendulumDamping = .08;
        PendulumStiffness = 60; PendulumMaxStretch = 3;
        CrossGatherRate = 120; CrossReach = 450; CrossBurstCount = 300;
        DiceSize = 76; DiceSensitivity = 1; DiceStopDelay = .18;
        RailChargePerNotch = 8; RailPower = 1; RailImpactCount = 200;
    }
    static double Clamp(double v, double low, double high, double fallback)
    { return Double.IsNaN(v) || Double.IsInfinity(v) ? fallback : Math.Max(low, Math.Min(high, v)); }
    public void Normalize()
    {
        if (Effect != "confetti" && Effect != "gravity" && Effect != "cluster" && Effect != "blackhole" && Effect != "pendulum" && Effect != "crossflash" && Effect != "railgun" && Effect != "dice") Effect = "cluster";
        DiceSize = Clamp(DiceSize <= 0 ? 76 : DiceSize, 40, 140, 76);
        DiceSensitivity = Clamp(DiceSensitivity <= 0 ? 1 : DiceSensitivity, .3, 3, 1);
        DiceStopDelay = Clamp(DiceStopDelay <= 0 ? .18 : DiceStopDelay, .08, .6, .18);
        ConfettiCount = Math.Max(20, Math.Min(1200, ConfettiCount));
        GravityCount = Math.Max(10, Math.Min(800, GravityCount));
        ClusterCount = Math.Max(20, Math.Min(1000, ClusterCount));
        ConfettiLife = Clamp(ConfettiLife, .5, 10, 3.4);
        ConfettiSpeed = Clamp(ConfettiSpeed, .2, 3, 1);
        GravityLife = Clamp(GravityLife, 1, 30, 11);
        GravityStrength = Clamp(GravityStrength, .1, 5, 1);
        CollisionRadius = Clamp(CollisionRadius, 4, 60, 16);
        ClusterFuse = Clamp(ClusterFuse, .1, 5, .9);
        ClusterSpeed = Clamp(ClusterSpeed, .1, 4, 1);
        SlingshotPower = Clamp(SlingshotPower <= 0 ? 6 : SlingshotPower, 1, 15, 6);
        ClusterGravity = Clamp(ClusterGravity <= 0 ? 600 : ClusterGravity, 50, 3000, 600);
        BlackHoleCount = Math.Max(30, Math.Min(800, BlackHoleCount <= 0 ? 180 : BlackHoleCount));
        BlackHoleFeedCount = Math.Max(0, Math.Min(200, BlackHoleFeedCount));
        BlackHoleStrength = Clamp(BlackHoleStrength <= 0 ? 1 : BlackHoleStrength, .1, 5, 1);
        BlackHoleGrowth = Clamp(BlackHoleGrowth <= 0 ? .08 : BlackHoleGrowth, .01, 1, .08);
        BlackHoleOrbitDamping = Clamp(BlackHoleOrbitDamping, 0, .3, .01);
        BlackHoleOrbitRatio = Clamp(BlackHoleOrbitRatio, 0, 1, .8);
        PendulumLength = Clamp(PendulumLength <= 0 ? 180 : PendulumLength, 80, 360, 180);
        PendulumGravity = Clamp(PendulumGravity <= 0 ? 1000 : PendulumGravity, 200, 3000, 1000);
        PendulumDamping = Clamp(PendulumDamping, 0, 1, .08);
        PendulumStiffness = Clamp(PendulumStiffness <= 0 ? 60 : PendulumStiffness, 10, 300, 60);
        PendulumMaxStretch = Clamp(PendulumMaxStretch <= 0 ? 3 : PendulumMaxStretch, 1.2, 5, 3);
        CrossGatherRate = Clamp(CrossGatherRate <= 0 ? 120 : CrossGatherRate, 20, 400, 120);
        CrossReach = Clamp(CrossReach <= 0 ? 450 : CrossReach, 100, 900, 450);
        CrossBurstCount = Math.Max(50, Math.Min(1200, CrossBurstCount <= 0 ? 300 : CrossBurstCount));
        RailChargePerNotch = Clamp(RailChargePerNotch <= 0 ? 8 : RailChargePerNotch, 1, 25, 8);
        RailPower = Clamp(RailPower <= 0 ? 1 : RailPower, .5, 2, 1);
        RailImpactCount = Math.Max(50, Math.Min(800, RailImpactCount <= 0 ? 200 : RailImpactCount));
        if (FireMode != "instant" && FireMode != "middle" && FireMode != "left" && FireMode != "space" && FireMode != "slingshot") FireMode = "instant";
        if (Palette != "neon" && Palette != "pastel" && Palette != "ice" && Palette != "warm") Palette = "neon";
    }
    public static string ConfigPath { get { return Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "settings.json"); } }
    public static string Encode(EffectSettings s)
    {
        s.Normalize();
        using (MemoryStream stream = new MemoryStream())
        {
            new DataContractJsonSerializer(typeof(EffectSettings)).WriteObject(stream, s);
            return Encoding.UTF8.GetString(stream.ToArray());
        }
    }
    public static EffectSettings Decode(string json)
    {
        EffectSettings s;
        using (MemoryStream stream = new MemoryStream(Encoding.UTF8.GetBytes(json)))
            s = (EffectSettings)new DataContractJsonSerializer(typeof(EffectSettings)).ReadObject(stream);
        if (s == null) throw new InvalidDataException("설정이 비어 있습니다.");
        if (!json.Contains("\"PendulumDamping\"")) s.PendulumDamping = .08;
        if (!json.Contains("\"BlackHoleOrbitDamping\"")) s.BlackHoleOrbitDamping = .01;
        if (!json.Contains("\"BlackHoleOrbitRatio\"")) s.BlackHoleOrbitRatio = .8;
        s.Normalize();
        if (!json.Contains("\"BlackHoleFeedCount\"")) s.BlackHoleFeedCount = Math.Max(6, s.BlackHoleCount / 12);
        return s;
    }
    public static EffectSettings Load()
    {
        if (!File.Exists(ConfigPath)) return new EffectSettings();
        return Decode(File.ReadAllText(ConfigPath));
    }
    public static void Save(EffectSettings settings)
    {
        string temp = ConfigPath + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            File.WriteAllText(temp, Encode(settings), System.Text.Encoding.UTF8);
            if (File.Exists(ConfigPath)) File.Replace(temp, ConfigPath, null);
            else File.Move(temp, ConfigPath);
        }
        finally { if (File.Exists(temp)) File.Delete(temp); }
    }
}

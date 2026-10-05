using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Security.Principal;
using LibreHardwareMonitor.Hardware;

namespace DockCpuTemp;

// Binary layout must match src/SystemTemps.cpp HoverDockCpuTempFile.
[StructLayout(LayoutKind.Sequential, Pack = 1)]
struct SharedPayload
{
    public uint Magic;      // 'HDCT' 0x54434448
    public uint Version;    // 1
    public uint Sequence;
    public int CpuPackageC; // -1 if unavailable
    public int CpuPackageW; // -1 if unavailable
    public ulong StampMs;   // Environment.TickCount64
    public int Status;      // 0=ok, 1=needAdmin, 2=noSensor
    public unsafe fixed byte NameUtf16[128]; // 64 UTF-16 chars, NUL padded
}

static class Program
{
    const uint kMagic = 0x54434448;
    const string TaskName = @"HoverDockCpuTemp";
    const string SingletonName = @"Global\HoverDockCpuTemp.Singleton";

    static string PayloadPath()
    {
        var dir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "LiquidGlassDock");
        Directory.CreateDirectory(dir);
        return Path.Combine(dir, "cpu-temp.bin");
    }

    static unsafe void SetName(ref SharedPayload p, string name)
    {
        fixed (byte* dest = p.NameUtf16)
        {
            new Span<byte>(dest, 128).Clear();
            if (string.IsNullOrEmpty(name)) return;
            var take = Math.Min(name.Length, 63);
            MemoryMarshal.AsBytes(name.AsSpan(0, take)).CopyTo(new Span<byte>(dest, 128));
        }
    }

    static void WritePayload(ref SharedPayload payload)
    {
        var path = PayloadPath();
        var tmp = path + ".tmp";
        var bytes = new byte[Marshal.SizeOf<SharedPayload>()];
        MemoryMarshal.Write(bytes, in payload);
        File.WriteAllBytes(tmp, bytes);
        File.Move(tmp, path, overwrite: true);
    }

    [STAThread]
    static int Main(string[] args)
    {
        bool once = Has(args, "--once");
        bool installTask = Has(args, "--install-task");
        bool runLoop = Has(args, "--run") || (!once && !installTask);
        bool isAdmin = new WindowsPrincipal(WindowsIdentity.GetCurrent())
            .IsInRole(WindowsBuiltInRole.Administrator);

        if (installTask)
            return InstallTask(isAdmin);

        using var single = new Mutex(true, SingletonName, out bool createdNew);
        if (!createdNew)
            return 0;

        if (!isAdmin)
        {
            var denied = new SharedPayload
            {
                Magic = kMagic,
                Version = 1,
                Sequence = 1,
                CpuPackageC = -1,
                CpuPackageW = -1,
                StampMs = (ulong)Environment.TickCount64,
                Status = 1,
            };
            SetName(ref denied, "need-admin");
            WritePayload(ref denied);
            if (once) return 1;
            // Brief linger so Dock can observe need-admin, then exit.
            Thread.Sleep(2500);
            return 1;
        }

        var computer = new Computer { IsCpuEnabled = true };
        try { computer.Open(); }
        catch (Exception ex)
        {
            Console.Error.WriteLine("Open failed: " + ex.Message);
            return 2;
        }

        var visitor = new UpdateVisitor();
        uint seq = 0;
        try
        {
            do
            {
                computer.Accept(visitor);
                var (tempC, watts, name, status) = PickCpuPackage(computer);
                var payload = new SharedPayload
                {
                    Magic = kMagic,
                    Version = 1,
                    Sequence = ++seq,
                    CpuPackageC = tempC,
                    CpuPackageW = watts,
                    StampMs = (ulong)Environment.TickCount64,
                    Status = status,
                };
                SetName(ref payload, name);
                WritePayload(ref payload);
                if (once)
                {
                    Console.WriteLine($"CPU {tempC} C / {watts} W via {name} status={status}");
                    break;
                }
                if (!runLoop) break;
                Thread.Sleep(1500);
            } while (true);
        }
        finally { computer.Close(); }
        return 0;
    }

    static bool Has(string[] args, string flag) =>
        args.Any(a => string.Equals(a, flag, StringComparison.OrdinalIgnoreCase));

    static int InstallTask(bool isAdmin)
    {
        if (!isAdmin)
        {
            // Relaunch elevated so the task is created with HIGHEST run level.
            var psi = new ProcessStartInfo
            {
                FileName = Environment.ProcessPath!,
                Arguments = "--install-task",
                UseShellExecute = true,
                Verb = "runas",
            };
            try
            {
                using var p = Process.Start(psi);
                p?.WaitForExit(60000);
                return p?.ExitCode ?? 5;
            }
            catch
            {
                return 5; // UAC cancelled
            }
        }

        var exe = Environment.ProcessPath!;
        RunSchtasks($"/Delete /F /TN \"{TaskName}\"");
        // ONLOGON + HIGHEST: after this one elevation, later logons start the helper
        // without another UAC prompt. /IT keeps an interactive token for PawnIO.
        var create = $"/Create /F /TN \"{TaskName}\" /SC ONLOGON /RL HIGHEST /IT /TR \"{exe} --run\"";
        var code = RunSchtasks(create);
        if (code != 0)
        {
            Console.Error.WriteLine("schtasks create failed: " + code);
            return code;
        }
        RunSchtasks($"/Run /TN \"{TaskName}\"");
        Console.WriteLine("Installed and started task " + TaskName);
        return 0;
    }

    static int RunSchtasks(string arguments)
    {
        using var p = Process.Start(new ProcessStartInfo
        {
            FileName = "schtasks.exe",
            Arguments = arguments,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
        });
        if (p == null) return -1;
        p.WaitForExit(30000);
        Console.WriteLine(p.StandardOutput.ReadToEnd().Trim());
        var err = p.StandardError.ReadToEnd().Trim();
        if (!string.IsNullOrEmpty(err)) Console.Error.WriteLine(err);
        return p.ExitCode;
    }

    static (int tempC, int watts, string name, int status) PickCpuPackage(Computer computer)
    {
        int bestTempScore = 0;
        float bestTemp = float.NaN;
        string bestTempName = "";
        int bestPowerScore = 0;
        float bestPower = float.NaN;

        void Consider(IHardware hw)
        {
            foreach (var sensor in hw.Sensors)
            {
                if (!sensor.Value.HasValue) continue;
                var label = sensor.Name.Trim().ToLowerInvariant();
                if (sensor.SensorType == SensorType.Temperature)
                {
                    int score = TempScore(label);
                    if (score > bestTempScore)
                    {
                        bestTempScore = score;
                        bestTemp = sensor.Value.Value;
                        bestTempName = sensor.Name;
                    }
                }
                else if (sensor.SensorType == SensorType.Power)
                {
                    int score = PowerScore(label);
                    if (score > bestPowerScore)
                    {
                        bestPowerScore = score;
                        bestPower = sensor.Value.Value;
                    }
                }
            }
            foreach (var sub in hw.SubHardware) Consider(sub);
        }
        foreach (var hw in computer.Hardware) Consider(hw);

        int t = (!float.IsNaN(bestTemp) && bestTemp > 1f && bestTemp < 120f)
            ? (int)Math.Round(bestTemp) : -1;
        int w = (!float.IsNaN(bestPower) && bestPower > 0.5f && bestPower < 500f)
            ? (int)Math.Round(bestPower) : -1;
        return (t, w, bestTempName, t >= 0 ? 0 : 2);
    }

    static int TempScore(string label)
    {
        if (label.Contains("distance")) return 0;
        if (label is "cpu package" or "core (tctl/tdie)" or "core (tdie)" or "package") return 100;
        if (label.Contains("tctl") || label.Contains("tdie")) return 90;
        if (label.Contains("package")) return 80;
        if (label.StartsWith("cpu") && label.Contains("die")) return 75;
        if (label.StartsWith("core #") || label.StartsWith("cpu core")) return 40;
        return 10;
    }

    static int PowerScore(string label)
    {
        if (label is "cpu package" or "package") return 100;
        if (label.Contains("package")) return 80;
        if (label.Contains("cpu")) return 40;
        return 10;
    }
}

sealed class UpdateVisitor : IVisitor
{
    public void VisitComputer(IComputer computer) => computer.Traverse(this);
    public void VisitHardware(IHardware hardware)
    {
        hardware.Update();
        foreach (var sub in hardware.SubHardware) sub.Accept(this);
    }
    public void VisitSensor(ISensor sensor) { }
    public void VisitParameter(IParameter parameter) { }
}


using System;
using System.Reflection;
using System.IO;
using System.Diagnostics;
using System.Security.Principal;

namespace AllunoVDDInstaller
{
    class Program
    {
        const string HardwareId = "root\\alluno\\vdd";
        const string ClassGuid = "{4D36E968-E325-11CE-BFC1-08002BE10318}";
        const string ClassName = "Display";
        const string InfName = "AllunoVDD.inf";
        const string CertName = "Alluno.cer";
        const string NefConc = "nefconc.exe";

        static void Main(string[] args)
        {
            if (!IsAdmin())
            {
                try
                {
                    ProcessStartInfo psi = new ProcessStartInfo();
                    psi.FileName = Assembly.GetExecutingAssembly().Location;
                    psi.Verb = "runas";
                    if (args.Length > 0)
                        psi.Arguments = string.Join(" ", args);
                    Process.Start(psi);
                }
                catch (Exception) { Console.WriteLine("Administrator rights required."); Pause(); }
                return;
            }

            string action = "install";
            if (args.Length > 0)
                action = args[0].ToLower();
            else
            {
                string exeName = Path.GetFileNameWithoutExtension(Assembly.GetExecutingAssembly().Location).ToLower();
                if (exeName.Contains("uninstall"))
                    action = "uninstall";
            }

            if (action == "uninstall")
                Uninstall();
            else
                Install();

            Pause();
        }

        static void Install()
        {
            string dir = GetDir();
            string certPath = Path.Combine(dir, CertName);
            string infPath = Path.Combine(dir, InfName);
            string nefconc = Path.Combine(dir, NefConc);

            Console.WriteLine("Alluno VDD - Installing...\n");

            if (!File.Exists(nefconc))
            {
                Console.WriteLine("  Error: nefconc.exe not found.");
                return;
            }

            // Install certificate
            if (File.Exists(certPath))
            {
                Console.WriteLine("  Installing certificate...");
                RunCmd("certutil", string.Format("-addstore -f Root \"{0}\"", certPath));
                RunCmd("certutil", string.Format("-addstore -f TrustedPublisher \"{0}\"", certPath));
            }

            // Remove existing device (if any)
            Console.WriteLine("  Removing existing device (if any)...");
            RunCmd(nefconc, string.Format("--remove-device-node --hardware-id \"{0}\" --class-guid \"{1}\"", HardwareId, ClassGuid));

            // Create device node
            Console.WriteLine("  Creating device node...");
            int ret = RunCmd(nefconc, string.Format("--create-device-node --class-name {0} --class-guid \"{1}\" --hardware-id \"{2}\"", ClassName, ClassGuid, HardwareId));
            if (ret != 0 && ret != 3010)
            {
                Console.WriteLine("  Failed to create device node.");
                return;
            }

            // Install driver
            Console.WriteLine("  Installing driver...");
            ret = RunCmd(nefconc, string.Format("--install-driver --inf-path \"{0}\"", infPath));
            if (ret == 3010)
            {
                Console.WriteLine("\nInstallation complete! A reboot is required.");
                return;
            }
            if (ret != 0)
            {
                Console.WriteLine("  Failed to install driver.");
                return;
            }

            Console.WriteLine("\nInstallation complete!");
        }

        static void Uninstall()
        {
            string dir = GetDir();
            string nefconc = Path.Combine(dir, NefConc);

            Console.WriteLine("Alluno VDD - Uninstalling...\n");

            if (File.Exists(nefconc))
            {
                Console.WriteLine("  Removing device...");
                RunCmd(nefconc, string.Format("--remove-device-node --hardware-id \"{0}\" --class-guid \"{1}\"", HardwareId, ClassGuid));
            }

            // Remove driver packages from store
            Console.WriteLine("  Removing driver packages...");
            RemoveDriverPackages();

            // Remove certificate
            Console.WriteLine("  Removing certificate...");
            RunCmd("certutil", "-delstore Root \"Alluno\"");
            RunCmd("certutil", "-delstore TrustedPublisher \"Alluno\"");

            Console.WriteLine("\nUninstall complete!");
        }

        static void RemoveDriverPackages()
        {
            try
            {
                ProcessStartInfo psi = new ProcessStartInfo();
                psi.FileName = "pnputil";
                psi.Arguments = "/enum-drivers";
                psi.UseShellExecute = false;
                psi.RedirectStandardOutput = true;
                psi.CreateNoWindow = true;
                Process p = Process.Start(psi);
                string output = p.StandardOutput.ReadToEnd();
                p.WaitForExit();

                string[] lines = output.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries);
                string currentOem = null;
                foreach (string line in lines)
                {
                    if (line.Trim().StartsWith("Published Name") || line.Trim().StartsWith("Published name"))
                    {
                        string[] parts = line.Split(new[] { ':' }, 2);
                        if (parts.Length > 1) currentOem = parts[1].Trim();
                    }
                    if (line.Contains("AllunoVDD") && currentOem != null)
                    {
                        RunCmd("pnputil", string.Format("/delete-driver {0} /uninstall /force", currentOem));
                        currentOem = null;
                    }
                }
            }
            catch (Exception) { }
        }

        static int RunCmd(string exe, string args)
        {
            try
            {
                ProcessStartInfo psi = new ProcessStartInfo();
                psi.FileName = exe;
                psi.Arguments = args;
                psi.WindowStyle = ProcessWindowStyle.Hidden;
                psi.CreateNoWindow = true;
                psi.UseShellExecute = false;
                psi.RedirectStandardOutput = true;
                Process p = Process.Start(psi);
                p.WaitForExit();
                string output = p.StandardOutput.ReadToEnd();
                if (!string.IsNullOrEmpty(output))
                    Console.WriteLine("    " + output.Trim());
                return p.ExitCode;
            }
            catch (Exception ex)
            {
                Console.WriteLine("    " + ex.Message);
                return -1;
            }
        }

        static bool IsAdmin()
        {
            WindowsIdentity identity = WindowsIdentity.GetCurrent();
            WindowsPrincipal principal = new WindowsPrincipal(identity);
            return principal.IsInRole(WindowsBuiltInRole.Administrator);
        }

        static void Pause()
        {
            Console.WriteLine("\nPress any key to exit...");
            try { Console.ReadKey(); } catch (Exception) { }
        }

        static string GetDir()
        {
            return AppDomain.CurrentDomain.BaseDirectory;
        }
    }
}

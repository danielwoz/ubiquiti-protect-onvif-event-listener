using System;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using OnvifRecorderInstaller.Services;

namespace OnvifRecorderInstaller.Pages;

public partial class InstallPage : Page {
    public enum Mode { Install, Upgrade, Uninstall }

    private readonly MainWindow _main;
    private readonly Mode _mode;
    private readonly CancellationTokenSource _cts = new();
    private bool _succeeded;
    private string _capturedVersion = "";

    public InstallPage(MainWindow main, Mode mode) {
        InitializeComponent();
        _main = main;
        _mode = mode;
        Header.Text = mode switch {
            Mode.Install   => "Installing onvif-recorder…",
            Mode.Upgrade   => "Checking for upgrades…",
            Mode.Uninstall => "Uninstalling onvif-recorder…",
            _ => "Running…",
        };
        Loaded += async (_, _) => await RunAsync();
    }

    private async Task RunAsync() {
        AppendLine($"$ # {_mode} on {_main.Connection.Host}");
        string script = _mode switch {
            Mode.Install => InstallScripts.BuildInstallScript(
                _main.Connection.Channel, _main.Connection.PinChannel),
            Mode.Upgrade => InstallScripts.BuildUpgradeScript(),
            Mode.Uninstall => InstallScripts.BuildUninstallScript(),
            _ => throw new InvalidOperationException(),
        };

        int status;
        try {
            status = await _main.Ssh.RunStreamAsync(
                _main.Connection,
                script,
                line => Dispatcher.Invoke(() => AppendLine(line)),
                _cts.Token);
        } catch (OperationCanceledException) {
            AppendLine("[cancelled]");
            Finish(success: false);
            return;
        } catch (Exception ex) {
            AppendLine($"[error] {ex.Message}");
            Finish(success: false);
            return;
        }
        Finish(success: status == 0);
    }

    private void AppendLine(string line) {
        LogBox.AppendText(line + "\r\n");
        LogBox.ScrollToEnd();

        // install/upgrade scripts end with a bare "dpkg-query -W -f='${Version}'"
        // call; capture the last non-empty line as the installed version.
        if (!string.IsNullOrWhiteSpace(line) && LooksLikeVersion(line)) {
            _capturedVersion = line.Trim();
        }
    }

    private static bool LooksLikeVersion(string line) {
        var t = line.Trim();
        if (t.Length == 0) return false;
        if (!char.IsDigit(t[0])) return false;
        foreach (var ch in t) {
            if (!(char.IsLetterOrDigit(ch) || ch == '.' || ch == '-'
                  || ch == '+' || ch == '~' || ch == ':')) return false;
        }
        return true;
    }

    private void Finish(bool success) {
        Progress.IsIndeterminate = false;
        Progress.Value = success ? 100 : 0;
        _succeeded = success;
        CancelButton.IsEnabled = false;
        Header.Text = _mode switch {
            Mode.Install when success   => "Installed successfully",
            Mode.Install                => "Install failed",
            Mode.Upgrade when success   => "Upgrade complete",
            Mode.Upgrade                => "Upgrade failed",
            Mode.Uninstall when success => "Uninstalled",
            Mode.Uninstall              => "Uninstall failed",
            _ => Header.Text,
        };

        if (success) {
            if (_mode == Mode.Install || _mode == Mode.Upgrade) {
                if (!string.IsNullOrEmpty(_capturedVersion)) {
                    _main.Connection.LastInstalledVersion = _capturedVersion;
                }
                _main.Store.Save(_main.Connection);
            }
            NextButton.IsEnabled = true;
        } else {
            CloseButton.IsEnabled = true;
        }
    }

    private void Next_Click(object sender, RoutedEventArgs e) {
        if (_mode == Mode.Install) {
            _main.Go(new DonePage(_main, _capturedVersion));
        } else {
            _main.Go(new DashboardPage(_main));
        }
    }

    private void Close_Click(object sender, RoutedEventArgs e) {
        Window.GetWindow(this)?.Close();
    }

    private void Cancel_Click(object sender, RoutedEventArgs e) {
        CancelButton.IsEnabled = false;
        AppendLine("[cancelling…]");
        _cts.Cancel();
    }
}

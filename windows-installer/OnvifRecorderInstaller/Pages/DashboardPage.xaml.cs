using System;
using System.Diagnostics;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using OnvifRecorderInstaller.Services;

namespace OnvifRecorderInstaller.Pages;

public partial class DashboardPage : Page {
    private readonly MainWindow _main;

    public DashboardPage(MainWindow main) {
        InitializeComponent();
        _main = main;
        HostText.Text = $"{main.Connection.Username}@{main.Connection.Host}";
        ChannelText.Text =
            $"Channel: {main.Catalog.Current.LabelFor(main.Connection.Channel)}";
        VersionText.Text = string.IsNullOrEmpty(main.Connection.LastInstalledVersion)
            ? "Version: (querying…)"
            : $"Version: {main.Connection.LastInstalledVersion}";
        Loaded += async (_, _) => await RefreshVersionAsync();
    }

    private async Task RefreshVersionAsync() {
        StatusText.Text = "Connecting…";
        RunResult result;
        try {
            result = await _main.Ssh.RunCaptureAsync(
                _main.Connection, InstallScripts.BuildVersionScript());
        } catch (Exception ex) {
            StatusText.Text = "Could not reach router: " + ex.Message;
            return;
        }
        if (!result.Ok) {
            StatusText.Text = "Could not reach router: " + result.Stderr;
            return;
        }
        var ver = result.Stdout.Trim();
        if (string.IsNullOrEmpty(ver)) {
            VersionText.Text = "Version: (not installed)";
            StatusText.Text =
                "onvif-recorder is not currently installed on this device.";
        } else {
            VersionText.Text = $"Version: {ver}";
            StatusText.Text = "";
            _main.Connection.LastInstalledVersion = ver;
            _main.Store.Save(_main.Connection);
        }
    }

    private void Upgrade_Click(object sender, RoutedEventArgs e) {
        _main.Go(new InstallPage(_main, InstallPage.Mode.Upgrade));
    }

    private void Reinstall_Click(object sender, RoutedEventArgs e) {
        // Through the channel page, so a re-install pins or follows Protect's
        // channel by the same rule as the first install.
        _main.Go(new ChannelPage(_main, fromDashboard: true));
    }

    private void OpenAdmin_Click(object sender, RoutedEventArgs e) {
        var host = _main.Connection.Host ?? "";
        if (Uri.CheckHostName(host) == UriHostNameType.Unknown) {
            MessageBox.Show(
                $"Saved host '{host}' is not a valid URL host.",
                "Can't open admin UI",
                MessageBoxButton.OK,
                MessageBoxImage.Warning);
            return;
        }
        var url = $"https://{host}/onvif/admin/";
        Process.Start(new ProcessStartInfo(url) { UseShellExecute = true });
    }

    private void Uninstall_Click(object sender, RoutedEventArgs e) {
        var answer = MessageBox.Show(
            $"Remove onvif-recorder from {_main.Connection.Host}?\n\n" +
            "This runs `apt-get purge` and removes the apt source list + keyring. " +
            "Your saved credentials will remain unless you choose to forget them afterward.",
            "Uninstall onvif-recorder",
            MessageBoxButton.OKCancel,
            MessageBoxImage.Warning);
        if (answer != MessageBoxResult.OK) return;
        _main.Go(new InstallPage(_main, InstallPage.Mode.Uninstall));
    }

    private void ChangeRouter_Click(object sender, RoutedEventArgs e) {
        var answer = MessageBox.Show(
            "Forget the saved router and start over?\n\n" +
            "This clears the stored host, credentials, and channel from the " +
            "Windows registry. It does NOT uninstall onvif-recorder from the " +
            "router itself.",
            "Change router",
            MessageBoxButton.OKCancel,
            MessageBoxImage.Question);
        if (answer != MessageBoxResult.OK) return;
        _main.Store.Forget();
        _main.Go(new WelcomePage(_main));
    }

    private void Close_Click(object sender, RoutedEventArgs e) {
        Window.GetWindow(this)?.Close();
    }
}

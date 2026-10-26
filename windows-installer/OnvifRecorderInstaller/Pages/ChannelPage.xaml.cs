using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using OnvifRecorderInstaller.Services;

namespace OnvifRecorderInstaller.Pages;

public partial class ChannelPage : Page {
    private readonly MainWindow _main;
    private readonly bool _fromDashboard;
    // Read from the device before Install is enabled; null when unknown.
    private string? _protectChannel;
    private string? _pinnedChannel;
    private bool _detected;

    public ChannelPage(MainWindow main, bool fromDashboard = false) {
        InitializeComponent();
        _main = main;
        _fromDashboard = fromDashboard;
        if (fromDashboard) HeaderText.Text = "Choose a release channel";
        Select(_main.Connection.Channel);
        _ = DetectAsync();
    }

    private async Task DetectAsync() {
        var reachable = await DetectProtectChannelAsync();
        if (reachable && !await ReadPinnedChannelAsync()) {
            // A pin that could not be read must not be removed by choosing
            // Protect's channel, so Protect's channel is treated as unknown
            // and the install pins whichever channel is chosen.
            _protectChannel = null;
            DetectedText.Text =
                "Could not read the release channel settings from the device.";
        }
        if (_pinnedChannel != null) {
            PinnedText.Text =
                $"onvif-recorder is pinned to {DisplayName(_pinnedChannel)} on this " +
                "device (chosen on its admin page or in an earlier install).";
            PinnedText.Visibility = Visibility.Visible;
        }
        Select(InstallScripts.DefaultChannel(
            _pinnedChannel, _protectChannel, _main.Connection.Channel));
        _detected = true;
        ChoicesPanel.IsEnabled = true;
        InstallButton.IsEnabled = true;
        UpdatePinHint();
    }

    // Sets _protectChannel and the detection message. Returns false when the
    // device could not be reached at all.
    private async Task<bool> DetectProtectChannelAsync() {
        RunResult result;
        try {
            result = await _main.Ssh.RunCaptureAsync(
                _main.Connection, InstallScripts.BuildDetectChannelScript());
        } catch (System.Exception ex) {
            DetectedText.Text =
                "Could not read release channel from the device (" + ex.Message + ").";
            return false;
        }
        if (!result.Ok) {
            DetectedText.Text = "Could not read release channel from the device.";
            return true;
        }
        _protectChannel = InstallScripts.ParseChannel(result.Stdout);
        DetectedText.Text = _protectChannel == null
            ? "Could not determine the Protect channel."
            : $"Detected Protect channel: {_protectChannel}";
        return true;
    }

    // Sets _pinnedChannel (null when the device is not pinned). Returns false
    // when the pin could not be read.
    private async Task<bool> ReadPinnedChannelAsync() {
        RunResult result;
        try {
            result = await _main.Ssh.RunCaptureAsync(
                _main.Connection, InstallScripts.BuildReadPinnedChannelScript());
        } catch (System.Exception) {
            return false;
        }
        if (!result.Ok) return false;
        _pinnedChannel = InstallScripts.ParseChannel(result.Stdout);
        return true;
    }

    private void UpdatePinHint() {
        if (!_detected) return;
        var channel = Selected();
        if (!InstallScripts.ShouldPinChannel(channel, _protectChannel, _pinnedChannel)) {
            PinHintText.Text =
                "onvif-recorder will follow Protect's release channel, including " +
                "any later change to it.";
        } else if (_protectChannel == null) {
            PinHintText.Text =
                "Protect's channel is unknown, so onvif-recorder will stay on " +
                $"{DisplayName(channel)} until you change it on the admin page.";
        } else {
            PinHintText.Text =
                $"onvif-recorder will stay on {DisplayName(channel)} even if " +
                "Protect's channel changes. You can change it on the admin page.";
        }
    }

    private static string DisplayName(string channel) => channel switch {
        "rc" => "Release Candidate",
        "early-access" => "Early Access",
        _ => "Stable",
    };

    private void Select(string channel) {
        StableRadio.IsChecked = channel == "stable";
        RcRadio.IsChecked = channel == "rc";
        EaRadio.IsChecked = channel == "early-access";
        if (StableRadio.IsChecked != true
            && RcRadio.IsChecked != true
            && EaRadio.IsChecked != true) {
            StableRadio.IsChecked = true;
        }
    }

    private string Selected() {
        if (RcRadio.IsChecked == true) return "rc";
        if (EaRadio.IsChecked == true) return "early-access";
        return "stable";
    }

    private void Channel_Checked(object sender, RoutedEventArgs e) {
        UpdatePinHint();
    }

    private void Back_Click(object sender, RoutedEventArgs e) {
        if (_fromDashboard) {
            _main.Go(new DashboardPage(_main));
        } else {
            _main.Go(new ConnectPage(_main));
        }
    }

    private void Install_Click(object sender, RoutedEventArgs e) {
        var channel = Selected();
        _main.Connection.Channel = channel;
        _main.Connection.PinChannel =
            InstallScripts.ShouldPinChannel(channel, _protectChannel, _pinnedChannel);
        _main.Go(new InstallPage(_main, InstallPage.Mode.Install));
    }
}

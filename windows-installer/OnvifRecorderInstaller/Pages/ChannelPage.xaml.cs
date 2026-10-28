using System;
using System.Diagnostics;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Navigation;
using OnvifRecorderInstaller.Services;

namespace OnvifRecorderInstaller.Pages;

public partial class ChannelPage : Page {
    private readonly MainWindow _main;
    private readonly bool _fromDashboard;
    private ChannelManifest _manifest;
    // Read from the device before Install is enabled; null when unknown.
    private string? _protectChannel;
    private string? _pinnedChannel;
    private string? _deviceChannel;
    private bool _packageInstalled;
    private bool _detected;

    public ChannelPage(MainWindow main, bool fromDashboard = false) {
        InitializeComponent();
        _main = main;
        _fromDashboard = fromDashboard;
        _manifest = main.Catalog.Current;
        if (fromDashboard) HeaderText.Text = "Choose a release channel";
        ChannelCombo.ItemsSource = _manifest.Channels;
        if (!Select(_main.Connection.Channel)) Select(_manifest.DefaultId);
        _ = DetectAsync();
    }

    private async Task DetectAsync() {
        var catalogTask = _main.Catalog.LoadAsync();
        var deviceTask = ReadDeviceChannelAsync();
        var reachable = await DetectProtectChannelAsync();
        if (reachable && !await ReadPinnedChannelAsync()) {
            // A pin that could not be read must not be removed by choosing
            // Protect's channel, so Protect's channel is treated as unknown
            // and the install pins whichever channel is chosen.
            _protectChannel = null;
            DetectedText.Text =
                "Could not read the release channel settings from the device.";
        }
        await deviceTask;
        var catalog = await catalogTask;

        _manifest = catalog.Manifest;
        _pinnedChannel = _manifest.Resolve(_pinnedChannel);
        _deviceChannel = _manifest.Resolve(_deviceChannel);
        ShowCatalogSource(catalog);
        // A pin or current channel the list does not name is still offered,
        // so it can be kept.
        ChannelCombo.ItemsSource = _manifest.WithExtraChannels(
            (_pinnedChannel, "current pin"),
            (_packageInstalled ? _deviceChannel : null, "current channel"));
        ShowDeviceChannel();

        var preferred = InstallScripts.DefaultChannel(
            _pinnedChannel, _deviceChannel, _packageInstalled, _manifest.DefaultId);
        if (!Select(preferred)) Select(_manifest.DefaultId);
        _detected = true;
        ChannelCombo.IsEnabled = true;
        UpdateSelection();
    }

    // Sets _protectChannel and the detection message. Returns false when the
    // device could not be reached at all.
    private async Task<bool> DetectProtectChannelAsync() {
        RunResult result;
        try {
            result = await _main.Ssh.RunCaptureAsync(
                _main.Connection, InstallScripts.BuildDetectChannelScript());
        } catch (Exception ex) {
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
            : $"Detected Protect release channel: {ProtectChannelName(_protectChannel)}.";
        return true;
    }

    // Sets _pinnedChannel (null when the device is not pinned). Returns false
    // when the pin could not be read.
    private async Task<bool> ReadPinnedChannelAsync() {
        RunResult result;
        try {
            result = await _main.Ssh.RunCaptureAsync(
                _main.Connection, InstallScripts.BuildReadPinnedChannelScript());
        } catch (Exception) {
            return false;
        }
        if (!result.Ok) return false;
        _pinnedChannel = InstallScripts.ParseChannel(result.Stdout);
        return true;
    }

    // Sets _packageInstalled and _deviceChannel; when the device cannot be
    // read they stay "not installed" and unknown.
    private async Task ReadDeviceChannelAsync() {
        try {
            var result = await _main.Ssh.RunCaptureAsync(
                _main.Connection, InstallScripts.BuildReadDeviceChannelScript());
            if (!result.Ok) return;
            (_packageInstalled, _deviceChannel) =
                InstallScripts.ParseDeviceChannel(result.Stdout);
        } catch (Exception) {
            // Treated as a device without onvif-recorder.
        }
    }

    private void ShowDeviceChannel() {
        string? text = null;
        if (_pinnedChannel != null) {
            text = $"onvif-recorder is pinned to {LabelOf(_pinnedChannel)} on this " +
                   "device (chosen on its admin page or in an earlier install).";
        } else if (_packageInstalled && _deviceChannel != null) {
            text = "onvif-recorder is installed on this device and uses the " +
                   $"{LabelOf(_deviceChannel)} channel.";
        }
        PinnedText.Text = text ?? "";
        PinnedText.Visibility = text == null ? Visibility.Collapsed : Visibility.Visible;
    }

    private void ShowCatalogSource(ChannelCatalogResult catalog) {
        var text = catalog.Source switch {
            ChannelSource.Router => "Channel list fetched through the router.",
            ChannelSource.Cached => catalog.FetchedUtc is { } at
                ? "Could not fetch the channel list; showing the list as of " +
                  $"{at.ToLocalTime():yyyy-MM-dd HH:mm}."
                : "Could not fetch the channel list; showing the last list fetched.",
            ChannelSource.BuiltIn =>
                "Could not fetch current versions; showing built-in channels.",
            _ => "",
        };
        CatalogSourceText.Text = text;
        CatalogSourceText.Visibility =
            text.Length == 0 ? Visibility.Collapsed : Visibility.Visible;

        UpdateText.Inlines.Clear();
        if (!catalog.InstallerOutdated) {
            UpdateText.Visibility = Visibility.Collapsed;
            return;
        }
        UpdateText.Inlines.Add(new Run(catalog.NewerSchemaSeen
            ? "This installer is out of date and cannot read the current channel " +
              "list. Download the latest installer: "
            : "A newer version of this installer is available: "));
        var url = catalog.Manifest.DownloadUrl;
        var link = new Hyperlink(new Run(url)) { NavigateUri = new Uri(url) };
        link.RequestNavigate += OpenLink;
        UpdateText.Inlines.Add(link);
        UpdateText.Visibility = Visibility.Visible;
    }

    private static void OpenLink(object sender, RequestNavigateEventArgs e) {
        Process.Start(new ProcessStartInfo(e.Uri.AbsoluteUri) { UseShellExecute = true });
        e.Handled = true;
    }

    private void UpdateSelection() {
        var selected = Selected();
        ChannelDescriptionText.Text = selected?.Description ?? "";
        InstallButton.IsEnabled = _detected && selected != null && selected.Selectable;
        UpdatePinHint(selected);
    }

    private void UpdatePinHint(ChannelOption? selected) {
        if (!_detected || selected == null) {
            PinHintText.Text = "";
            return;
        }
        if (!InstallScripts.ShouldPinChannel(selected.Id, _protectChannel, _pinnedChannel)) {
            PinHintText.Text =
                "onvif-recorder will follow Protect's release channel, including " +
                "any later change to it.";
        } else if (_protectChannel == null) {
            PinHintText.Text =
                "Protect's channel is unknown, so onvif-recorder will stay on " +
                $"{selected.Label} until you change it on the admin page.";
        } else {
            PinHintText.Text =
                $"onvif-recorder will stay on {selected.Label} even if " +
                "Protect's channel changes. You can change it on the admin page.";
        }
    }

    private string LabelOf(string channel) => _manifest.LabelFor(channel);

    // Protect's own names for the channels detect-channel.sh maps to suites.
    // early-access stands for every pre-release channel mapped to it.
    private static string ProtectChannelName(string channel) => channel switch {
        "stable" => "Release",
        "rc" => "Release Candidate",
        "early-access" => "Early Access or Alpha",
        _ => channel,
    };

    // Selects the selectable entry with this id; false when there is none.
    private bool Select(string? id) {
        foreach (var item in ChannelCombo.Items) {
            if (item is ChannelOption option && option.Id == id && option.Selectable) {
                ChannelCombo.SelectedItem = option;
                return true;
            }
        }
        return false;
    }

    private ChannelOption? Selected() => ChannelCombo.SelectedItem as ChannelOption;

    private void ChannelCombo_SelectionChanged(object sender, SelectionChangedEventArgs e) {
        UpdateSelection();
    }

    private void Back_Click(object sender, RoutedEventArgs e) {
        if (_fromDashboard) {
            _main.Go(new DashboardPage(_main));
        } else {
            _main.Go(new ConnectPage(_main));
        }
    }

    private void Install_Click(object sender, RoutedEventArgs e) {
        var selected = Selected();
        if (selected == null || !selected.Selectable) return;
        _main.Connection.Channel = selected.Id;
        _main.Connection.PinChannel = InstallScripts.ShouldPinChannel(
            selected.Id, _protectChannel, _pinnedChannel);
        _main.Go(new InstallPage(_main, InstallPage.Mode.Install));
    }
}

using System;
using OnvifRecorderInstaller.Services;
using Xunit;

namespace OnvifRecorderInstaller.Tests;

public class InstallScriptsTests {
    [Theory]
    [InlineData("stable", false)]
    [InlineData("rc", false)]
    [InlineData("early-access", false)]
    [InlineData("stable", true)]
    [InlineData("rc", true)]
    [InlineData("early-access", true)]
    public void BuildInstallScript_IncludesChannel(string channel, bool pin) {
        var script = InstallScripts.BuildInstallScript(channel, pin);
        Assert.Contains($"echo \"{channel}\" > {InstallScripts.ChannelFile}", script);
        Assert.Contains($"{InstallScripts.RepoUrl} {channel} main", script);
    }

    // admin_server.cpp's channel Apply writes "<channel>\n" to the channel
    // file's path plus ".pinned"; detect-channel.sh reads that path.
    [Fact]
    public void PinnedChannelFile_IsChannelFileWithPinnedSuffix() {
        Assert.Equal(InstallScripts.ChannelFile + ".pinned",
                     InstallScripts.PinnedChannelFile);
    }

    [Theory]
    [InlineData("stable")]
    [InlineData("rc")]
    [InlineData("early-access")]
    public void BuildInstallScript_PinWritesMarkerLikeAdminPage(string channel) {
        var script = InstallScripts.BuildInstallScript(channel, pinChannel: true);
        Assert.Contains(
            $"echo \"{channel}\" > {InstallScripts.PinnedChannelFile}", script);
        Assert.DoesNotContain($"rm -f {InstallScripts.PinnedChannelFile}", script);
        Assert.Contains($"==> Channel: {channel} (pinned)", script);
    }

    [Fact]
    public void BuildInstallScript_NoPinRemovesEarlierMarker() {
        var script = InstallScripts.BuildInstallScript("stable", pinChannel: false);
        Assert.Contains($"rm -f {InstallScripts.PinnedChannelFile}", script);
        Assert.DoesNotContain($"> {InstallScripts.PinnedChannelFile}", script);
        Assert.Contains("follows the Protect release channel", script);
    }

    // postinst runs detect-channel.sh during apt-get install, so the marker
    // has to be in place (or gone) before the package is installed.
    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public void BuildInstallScript_SetsMarkerBeforeInstallingPackage(bool pin) {
        var script = InstallScripts.BuildInstallScript("rc", pin);
        var channelAt = script.IndexOf(
            $"> {InstallScripts.ChannelFile}", StringComparison.Ordinal);
        var markerAt = script.IndexOf(
            InstallScripts.PinnedChannelFile, StringComparison.Ordinal);
        var installAt = script.IndexOf(
            "apt-get install -y onvif-recorder", StringComparison.Ordinal);
        Assert.True(channelAt >= 0);
        Assert.True(markerAt > channelAt);
        Assert.True(installAt > markerAt);
    }

    [Fact]
    public void BuildInstallScript_ReferencesKeyringPath() {
        var script = InstallScripts.BuildInstallScript("stable", false);
        Assert.Contains(InstallScripts.Keyring, script);
        Assert.Contains($"{InstallScripts.RepoUrl}/onvif-recorder.gpg", script);
    }

    // The channel goes into a root shell script and the apt source line.
    [Theory]
    [InlineData("early-access; rm -rf /", true)]
    [InlineData("early-access; rm -rf /", false)]
    [InlineData("$(reboot)", true)]
    [InlineData("`reboot`", false)]
    [InlineData("rc\n", true)]
    [InlineData("rc\nreboot", false)]
    [InlineData("rc rc", true)]
    [InlineData("a'b", false)]
    [InlineData("a\"b", true)]
    [InlineData("Stable", false)]
    [InlineData("STABLE", true)]
    [InlineData("-rc", false)]
    [InlineData("rc-", true)]
    [InlineData("r", false)]
    [InlineData("", true)]
    [InlineData("abcdefghijklmnopqrstuvwxyzabcdefg", false)]
    public void BuildInstallScript_RejectsUnsafeChannel(string channel, bool pin) {
        Assert.Throws<ArgumentException>(() =>
            InstallScripts.BuildInstallScript(channel, pin));
    }

    // A channel that is not one of today's three, such as a pin written by
    // a newer installer, still installs.
    [Fact]
    public void BuildInstallScript_AcceptsSafeChannelOutsideTheList() {
        var script = InstallScripts.BuildInstallScript("weekly", pinChannel: true);
        Assert.Contains($"{InstallScripts.RepoUrl} weekly main", script);
    }

    [Theory]
    [InlineData("stable", true)]
    [InlineData("rc", true)]
    [InlineData("early-access", true)]
    [InlineData("latest", true)]
    [InlineData("a1", true)]
    [InlineData("abcdefghijklmnopqrstuvwxyzabcdef", true)]
    [InlineData("abcdefghijklmnopqrstuvwxyzabcdefg", false)]
    [InlineData("r", false)]
    [InlineData("1rc", false)]
    [InlineData("-rc", false)]
    [InlineData("rc-", false)]
    [InlineData("rc_1", false)]
    [InlineData("rc.1", false)]
    [InlineData("Rc", false)]
    [InlineData("rc\n", false)]
    [InlineData(" rc", false)]
    [InlineData("éa", false)]
    [InlineData("", false)]
    [InlineData(null, false)]
    public void IsSafeChannelId_AllowsOnlyShellSafeIds(string? id, bool expected) {
        Assert.Equal(expected, InstallScripts.IsSafeChannelId(id));
    }

    [Fact]
    public void BuildReadPinnedChannelScript_ReadsMarker() {
        var script = InstallScripts.BuildReadPinnedChannelScript();
        Assert.Contains($"cat {InstallScripts.PinnedChannelFile}", script);
    }

    [Theory]
    [InlineData("stable\n", "stable")]
    [InlineData("  rc \r\n", "rc")]
    [InlineData("early-access", "early-access")]
    [InlineData("weekly", "weekly")]
    [InlineData("", null)]
    [InlineData("\n", null)]
    [InlineData("Stable", null)]
    [InlineData("rc; reboot", null)]
    [InlineData("rc\nstable", null)]
    [InlineData(null, null)]
    public void ParseChannel_AcceptsOnlySafeIds(string? output, string? expected) {
        Assert.Equal(expected, InstallScripts.ParseChannel(output));
    }

    [Fact]
    public void ChannelsUrl_IsNextToTheAptRepo() {
        Assert.Equal(InstallScripts.RepoUrl + "/channels.json", InstallScripts.ChannelsUrl);
    }

    [Fact]
    public void BuildFetchManifestScript_FetchesChannelsJsonWithLimits() {
        var script = InstallScripts.BuildFetchManifestScript();
        Assert.Contains($"'{InstallScripts.ChannelsUrl}'", script);
        Assert.Contains("curl -fsS -m 6", script);
        Assert.Contains($"--max-filesize {ChannelManifest.MaxBytes}", script);
        Assert.Contains($"head -c {ChannelManifest.MaxBytes + 1}", script);
    }

    [Fact]
    public void BuildReadDeviceChannelScript_ReadsPackageStateAndChannel() {
        var script = InstallScripts.BuildReadDeviceChannelScript();
        Assert.Contains("dpkg-query -W -f='${Status}' onvif-recorder", script);
        Assert.Contains($"cat {InstallScripts.ChannelFile}", script);
    }

    [Theory]
    [InlineData("installed\nstable\n", true, "stable")]
    [InlineData("installed\r\nrc\r\n", true, "rc")]
    [InlineData("not-installed\nearly-access\n", false, "early-access")]
    [InlineData("installed\n\n", true, null)]
    [InlineData("installed\nStable; reboot\n", true, null)]
    [InlineData("installed", true, null)]
    [InlineData("", false, null)]
    [InlineData(null, false, null)]
    public void ParseDeviceChannel_ReadsBothLines(
            string? output, bool installed, string? channel) {
        var parsed = InstallScripts.ParseDeviceChannel(output);
        Assert.Equal(installed, parsed.Installed);
        Assert.Equal(channel, parsed.Channel);
    }

    [Theory]
    // Protect's channel, nothing pinned: follow Protect.
    [InlineData("stable", "stable", null, false)]
    [InlineData("early-access", "early-access", null, false)]
    // Latest (early-access) on a device whose Protect is on Release: pin.
    [InlineData("early-access", "stable", null, true)]
    // A different channel from Protect's: pin.
    [InlineData("stable", "rc", null, true)]
    // Protect's channel unknown (or the device's pin unreadable, which the
    // channel page passes as an unknown Protect channel): pin whatever was
    // chosen, so a pin that could not be read is never removed.
    [InlineData("stable", null, null, true)]
    [InlineData("rc", null, null, true)]
    // Keeping the channel already pinned on the device keeps the pin.
    [InlineData("stable", "stable", "stable", true)]
    [InlineData("rc", null, "rc", true)]
    // Moving from a pinned channel to Protect's: follow Protect.
    [InlineData("stable", "stable", "early-access", false)]
    // Moving from a pinned channel to a third one: pin the new one.
    [InlineData("rc", "stable", "early-access", true)]
    public void ShouldPinChannel_PinsUnlessFollowingProtect(
            string channel, string? protect, string? pinned, bool expected) {
        Assert.Equal(expected,
                     InstallScripts.ShouldPinChannel(channel, protect, pinned));
    }

    [Theory]
    // The device's pin wins.
    [InlineData("rc", "stable", true, "early-access", "rc")]
    [InlineData("stable", null, false, "early-access", "stable")]
    // A device with onvif-recorder installed keeps its current channel.
    [InlineData(null, "stable", true, "early-access", "stable")]
    [InlineData(null, "rc", true, "early-access", "rc")]
    // A device without it (or with only the channel file left by a
    // removed package) gets the channel list's default.
    [InlineData(null, "stable", false, "early-access", "early-access")]
    [InlineData(null, null, true, "rc", "rc")]
    [InlineData(null, null, false, "stable", "stable")]
    // No usable default: Latest (early-access).
    [InlineData(null, null, false, null, "early-access")]
    [InlineData(null, null, false, "rc; reboot", "early-access")]
    // Unsafe values are skipped.
    [InlineData("Stable", "rc", true, "early-access", "rc")]
    public void DefaultChannel_PrefersPinThenInstalledChannelThenListDefault(
            string? pinned, string? device, bool installed, string? listDefault,
            string expected) {
        Assert.Equal(expected,
                     InstallScripts.DefaultChannel(pinned, device, installed, listDefault));
    }

    [Fact]
    public void DefaultChannelId_IsLatest() {
        Assert.Equal("early-access", InstallScripts.DefaultChannelId);
        Assert.Equal(InstallScripts.DefaultChannelId, new Connection().Channel);
    }

    [Fact]
    public void BuildUpgradeScript_UsesOnlyUpgrade() {
        var script = InstallScripts.BuildUpgradeScript();
        Assert.Contains("apt-get install -y --only-upgrade onvif-recorder", script);
        Assert.Contains(InstallScripts.SourcesFile, script);
    }

    [Fact]
    public void BuildUninstallScript_PurgesAndCleansUp() {
        var script = InstallScripts.BuildUninstallScript();
        Assert.Contains("apt-get purge -y onvif-recorder", script);
        Assert.Contains($"rm -f {InstallScripts.SourcesFile}", script);
        Assert.Contains($"rm -f {InstallScripts.Keyring}", script);
    }

    [Fact]
    public void BuildDetectChannelScript_MapsProtectChannelToAptSuite() {
        var script = InstallScripts.BuildDetectChannelScript();
        Assert.Contains(InstallScripts.RunnablesYaml, script);
        Assert.Contains("release)", script);
        Assert.Contains("release-candidate)", script);
        Assert.Contains("beta|early-access|alpha|internal|qa)", script);
        Assert.Contains("echo \"stable\"", script);
        Assert.Contains("echo \"rc\"", script);
        Assert.Contains("echo \"early-access\"", script);
    }
}

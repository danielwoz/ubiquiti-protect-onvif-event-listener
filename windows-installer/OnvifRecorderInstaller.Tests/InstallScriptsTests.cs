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

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public void BuildInstallScript_RejectsUnknownChannel(bool pin) {
        Assert.Throws<ArgumentException>(() =>
            InstallScripts.BuildInstallScript("weekly", pin));
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
    [InlineData("", null)]
    [InlineData("\n", null)]
    [InlineData("weekly", null)]
    [InlineData(null, null)]
    public void ParseChannel_AcceptsOnlyKnownChannels(string? output, string? expected) {
        Assert.Equal(expected, InstallScripts.ParseChannel(output));
    }

    [Theory]
    // Protect's channel, nothing pinned: follow Protect.
    [InlineData("stable", "stable", null, false)]
    [InlineData("early-access", "early-access", null, false)]
    // A different channel from Protect's: pin.
    [InlineData("early-access", "stable", null, true)]
    [InlineData("stable", "rc", null, true)]
    // Protect's channel unknown: pin whatever was chosen.
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
    [InlineData("rc", "stable", "early-access", "rc")]
    [InlineData(null, "early-access", "stable", "early-access")]
    [InlineData(null, null, "rc", "rc")]
    [InlineData(null, null, "weekly", "stable")]
    [InlineData(null, null, "", "stable")]
    public void DefaultChannel_PrefersPinThenProtectThenSaved(
            string? pinned, string? protect, string saved, string expected) {
        Assert.Equal(expected,
                     InstallScripts.DefaultChannel(pinned, protect, saved));
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
        Assert.Contains("beta|early-access)", script);
        Assert.Contains("echo \"stable\"", script);
        Assert.Contains("echo \"rc\"", script);
        Assert.Contains("echo \"early-access\"", script);
    }
}

using System;
using System.Globalization;
using System.Runtime.Versioning;
using Microsoft.Win32;

namespace OnvifRecorderInstaller.Services;

// Keeps the last valid channels.json under
// HKCU\Software\OnvifRecorderInstaller (ChannelsJson, ChannelsFetchedUtc),
// next to the saved connection. CredentialStore.Forget() removes it too.
[SupportedOSPlatform("windows")]
public sealed class RegistryChannelCache : IChannelCache {
    private const string JsonValue = "ChannelsJson";
    private const string FetchedValue = "ChannelsFetchedUtc";

    public CachedManifest? Read() {
        using var key = Registry.CurrentUser.OpenSubKey(CredentialStore.SubKeyPath);
        if (key?.GetValue(JsonValue) is not string json || json.Length == 0) return null;
        DateTimeOffset? fetched = null;
        if (key.GetValue(FetchedValue) is string text
            && DateTimeOffset.TryParse(text, CultureInfo.InvariantCulture,
                                       DateTimeStyles.AssumeUniversal, out var when)) {
            fetched = when;
        }
        return new CachedManifest(json, fetched);
    }

    public void Write(string json, DateTimeOffset fetchedUtc) {
        using var key = Registry.CurrentUser.CreateSubKey(CredentialStore.SubKeyPath);
        key.SetValue(JsonValue, json, RegistryValueKind.String);
        key.SetValue(FetchedValue,
                     fetchedUtc.ToUniversalTime().ToString("o", CultureInfo.InvariantCulture),
                     RegistryValueKind.String);
    }
}

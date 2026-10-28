using System;
using System.Runtime.Versioning;
using System.Security.Cryptography;
using System.Text;
using Microsoft.Win32;

namespace OnvifRecorderInstaller.Services;

// Reads and writes the installer's connection + channel state under
// HKCU\Software\OnvifRecorderInstaller (RegistryChannelCache keeps the last
// channel list in the same key). Secrets are DPAPI-encrypted with a
// fixed entropy constant so a raw registry dump is not sufficient to recover
// them; they can only be decrypted by the same Windows user.
[SupportedOSPlatform("windows")]
public sealed class CredentialStore {
    public const string SubKeyPath = @"Software\OnvifRecorderInstaller";

    // Fixed entropy so ProtectedData is tied to this app, not just the user.
    // Changing this string invalidates all previously stored secrets.
    private static readonly byte[] Entropy =
        Encoding.UTF8.GetBytes("onvif-recorder-installer/v1");

    public bool Exists() {
        using var key = Registry.CurrentUser.OpenSubKey(SubKeyPath);
        return key != null && key.GetValue("Host") != null;
    }

    public Connection? Load() {
        using var key = Registry.CurrentUser.OpenSubKey(SubKeyPath);
        if (key == null) return null;

        var host = key.GetValue("Host") as string;
        if (string.IsNullOrEmpty(host)) return null;

        var c = new Connection {
            Host = host,
            Port = (int)(key.GetValue("Port") ?? 22),
            Username = (key.GetValue("Username") as string) ?? "root",
            AuthMethod = Enum.TryParse<AuthMethod>(
                    key.GetValue("AuthMethod") as string, out var m)
                ? m : AuthMethod.Password,
            HostFingerprint = key.GetValue("HostFingerprint") as string,
            Channel = (key.GetValue("Channel") as string) ?? InstallScripts.DefaultChannelId,
            LastInstalledVersion = key.GetValue("LastInstalledVer") as string,
        };

        if (c.AuthMethod == AuthMethod.Password) {
            c.Password = Unprotect(key.GetValue("PasswordEnc") as byte[]);
        } else {
            c.PrivateKeyPath = key.GetValue("KeyPath") as string;
            c.PrivateKeyPassphrase =
                Unprotect(key.GetValue("KeyPassphraseEnc") as byte[]);
        }
        return c;
    }

    public void Save(Connection c) {
        using var key = Registry.CurrentUser.CreateSubKey(SubKeyPath);
        key.SetValue("Host", c.Host, RegistryValueKind.String);
        key.SetValue("Port", c.Port, RegistryValueKind.DWord);
        key.SetValue("Username", c.Username, RegistryValueKind.String);
        key.SetValue("AuthMethod", c.AuthMethod.ToString(),
                     RegistryValueKind.String);
        key.SetValue("Channel", c.Channel, RegistryValueKind.String);

        if (!string.IsNullOrEmpty(c.HostFingerprint)) {
            key.SetValue("HostFingerprint", c.HostFingerprint,
                         RegistryValueKind.String);
        }
        if (!string.IsNullOrEmpty(c.LastInstalledVersion)) {
            key.SetValue("LastInstalledVer", c.LastInstalledVersion,
                         RegistryValueKind.String);
        }

        if (c.AuthMethod == AuthMethod.Password) {
            SetOrClear(key, "PasswordEnc", Protect(c.Password));
            ClearIfPresent(key, "KeyPath");
            ClearIfPresent(key, "KeyPassphraseEnc");
        } else {
            if (!string.IsNullOrEmpty(c.PrivateKeyPath)) {
                key.SetValue("KeyPath", c.PrivateKeyPath,
                             RegistryValueKind.String);
            }
            SetOrClear(key, "KeyPassphraseEnc",
                       Protect(c.PrivateKeyPassphrase));
            ClearIfPresent(key, "PasswordEnc");
        }
    }

    public void Forget() {
        Registry.CurrentUser.DeleteSubKeyTree(SubKeyPath, throwOnMissingSubKey: false);
    }

    // Exposed so tests can construct realistic fixtures without going through
    // Save/Load. Not used by the production code paths.
    internal static byte[]? Protect(string? plaintext) {
        if (string.IsNullOrEmpty(plaintext)) return null;
        return ProtectedData.Protect(
            Encoding.UTF8.GetBytes(plaintext),
            Entropy,
            DataProtectionScope.CurrentUser);
    }

    internal static string? Unprotect(byte[]? ciphertext) {
        if (ciphertext == null || ciphertext.Length == 0) return null;
        try {
            var bytes = ProtectedData.Unprotect(
                ciphertext, Entropy, DataProtectionScope.CurrentUser);
            return Encoding.UTF8.GetString(bytes);
        } catch (CryptographicException) {
            return null;
        }
    }

    private static void SetOrClear(RegistryKey key, string name, byte[]? value) {
        if (value == null || value.Length == 0) {
            ClearIfPresent(key, name);
        } else {
            key.SetValue(name, value, RegistryValueKind.Binary);
        }
    }

    private static void ClearIfPresent(RegistryKey key, string name) {
        if (key.GetValue(name) != null) key.DeleteValue(name, false);
    }
}

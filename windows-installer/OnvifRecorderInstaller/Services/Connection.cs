using System;
using System.Security;

namespace OnvifRecorderInstaller.Services;

public enum AuthMethod {
    Password,
    PrivateKey,
}

// Plain-text credential holder. Only lives in memory during a session; the
// persisted form goes through CredentialStore which DPAPI-encrypts the
// secret.
public sealed class Connection {
    public string Host { get; set; } = "192.168.1.1";
    public int Port { get; set; } = 22;
    public string Username { get; set; } = "root";
    public AuthMethod AuthMethod { get; set; } = AuthMethod.Password;
    public string? Password { get; set; }
    public string? PrivateKeyPath { get; set; }
    public string? PrivateKeyPassphrase { get; set; }
    public string? HostFingerprint { get; set; }
    public string Channel { get; set; } = "stable";
    // Whether the next install pins Channel on the device. Set by the
    // channel page before every install; not persisted.
    public bool PinChannel { get; set; }
    public string? LastInstalledVersion { get; set; }

    public bool HasCredentials =>
        AuthMethod == AuthMethod.Password
            ? !string.IsNullOrEmpty(Password)
            : !string.IsNullOrEmpty(PrivateKeyPath);
}

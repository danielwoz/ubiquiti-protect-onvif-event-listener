using System.Net.Http;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using OnvifRecorderInstaller.Pages;
using OnvifRecorderInstaller.Services;

namespace OnvifRecorderInstaller;

public partial class MainWindow : Window {
    public CredentialStore Store { get; } = new();
    public SshService Ssh { get; } = new();
    public Connection Connection { get; private set; } = new();
    // The release channel list. The HTTPS fetch starts here so it is
    // usually done by the time the channel page needs it.
    public ChannelCatalog Catalog { get; }

    public MainWindow() {
        InitializeComponent();
        Catalog = new ChannelCatalog(
            new HttpClientHandler(), FetchChannelsThroughRouterAsync,
            new RegistryChannelCache(), ChannelCatalog.AssemblyVersion());
        Catalog.StartRemoteFetch();
        var saved = Store.Load();
        if (saved != null && saved.HasCredentials) {
            Connection = saved;
            RootFrame.Navigate(new DashboardPage(this));
        } else {
            RootFrame.Navigate(new WelcomePage(this));
        }
    }

    public void Go(System.Windows.Controls.Page page) {
        RootFrame.Navigate(page);
    }

    // channels.json as the router fetches it, for a PC that cannot reach
    // the repository itself; null before a router is set up.
    private async Task<string?> FetchChannelsThroughRouterAsync(CancellationToken ct) {
        if (!Connection.HasCredentials) return null;
        var result = await Ssh.RunCaptureAsync(
            Connection, InstallScripts.BuildFetchManifestScript(), ct);
        return result.Ok ? result.Stdout : null;
    }
}

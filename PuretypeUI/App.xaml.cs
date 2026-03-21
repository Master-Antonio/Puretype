using System.Threading;
using System.Windows;

namespace PuretypeUI
{
    public partial class App : Application
    {
        // Mutex name must match what injector.cpp checks with OpenMutexW.
        private const string MutexName = "PureTypeUI_Instance";
        private Mutex? _mutex;

        protected override void OnStartup(StartupEventArgs e)
        {
            // ── Single-instance guard ─────────────────────────────────────────
            _mutex = new Mutex(initiallyOwned: true, MutexName, out bool createdNew);
            if (!createdNew)
            {
                // Another instance is already running — the tray injector will
                // re-focus it via FindWindow. Just exit silently.
                _mutex.Dispose();
                _mutex = null;
                Shutdown();
                return;
            }

            // ── Global error handlers ─────────────────────────────────────────
            AppDomain.CurrentDomain.UnhandledException += (s, ex) =>
                MessageBox.Show($"Fatal error:\n{ex.ExceptionObject}",
                                "PureType — Unhandled Exception",
                                MessageBoxButton.OK, MessageBoxImage.Error);

            DispatcherUnhandledException += (s, ex) =>
            {
                MessageBox.Show($"UI error:\n{ex.Exception.Message}\n\n{ex.Exception.StackTrace}",
                                "PureType — UI Exception",
                                MessageBoxButton.OK, MessageBoxImage.Error);
                ex.Handled = true;
            };

            base.OnStartup(e);
        }

        protected override void OnExit(ExitEventArgs e)
        {
            if (_mutex != null)
            {
                try { _mutex.ReleaseMutex(); } catch { /* already released */ }
                _mutex.Dispose();
                _mutex = null;
            }
            base.OnExit(e);
        }
    }
}

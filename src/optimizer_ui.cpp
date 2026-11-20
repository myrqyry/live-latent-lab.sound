#include "optimizer_ui.h"
// Minimal forward-declarations for OBS frontend API used here to avoid
// depending on frontend headers being installed in the include path during build.
extern "C" {
    typedef void (*obs_frontend_cb)(void *private_data);
    void obs_frontend_add_tools_menu_item(const char *name, obs_frontend_cb callback, void *private_data);
}
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QProcess>
#include <QCoreApplication>
#include <QSettings>
#include <QDesktopServices>
#include <QUrl>

// Simple dialog that allows launching the external Python GUI (or future native actions)
class OptimizerDialog : public QDialog {
    Q_OBJECT

public:
    explicit OptimizerDialog(QWidget *parent = nullptr) : QDialog(parent) {
        setWindowTitle("RVC Model Optimizer");
        setMinimumSize(480, 160);

        auto *lay = new QVBoxLayout(this);

        auto *lbl = new QLabel("Launch the RVC Optimizer application.\nYou can configure the Python launcher path below.");
        lbl->setWordWrap(true);
        lay->addWidget(lbl);

        auto *hbox = new QHBoxLayout();
        pathEdit = new QLineEdit(this);
        pathEdit->setPlaceholderText("python3 /path/to/repo/rvc_optimizer_app/main.py");
        hbox->addWidget(pathEdit);

        auto *browseBtn = new QPushButton("Reset", this);
        connect(browseBtn, &QPushButton::clicked, this, [this]() {
            // reset to the default launcher command
            pathEdit->setText("python3 " + QCoreApplication::applicationDirPath() + "/../rvc_optimizer_app/main.py");
        });
        hbox->addWidget(browseBtn);
        lay->addLayout(hbox);

        auto *btns = new QHBoxLayout();
        auto *launch = new QPushButton("Launch Optimizer", this);
        connect(launch, &QPushButton::clicked, this, &OptimizerDialog::onLaunchClicked);
        btns->addWidget(launch);

        auto *close = new QPushButton("Close", this);
        connect(close, &QPushButton::clicked, this, &QDialog::accept);
        btns->addWidget(close);

        lay->addLayout(btns);

        // load saved path
        QSettings s("live-latent-lab-sound", "rvc_optimizer");
        QString saved = s.value("launcher_cmd").toString();
        if (!saved.isEmpty())
            pathEdit->setText(saved);
    }

private slots:
    void onLaunchClicked() {
        QString cmd = pathEdit->text().trimmed();
        if (cmd.isEmpty()) {
            return;
        }

        // persist
        QSettings s("live-latent-lab-sound", "rvc_optimizer");
        s.setValue("launcher_cmd", cmd);

        // Start via shell so users can configure complex commands
        QProcess *proc = new QProcess();
        proc->setProcessChannelMode(QProcess::MergedChannels);
        connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                proc, &QObject::deleteLater);
        connect(proc, &QProcess::errorOccurred, proc, &QObject::deleteLater);
        proc->start("/bin/sh", QStringList() << "-c" << cmd);
    }

private:
    QLineEdit *pathEdit;
};

extern "C" void optimizer_tools_cb(void *private_data)
{
    // Called from main thread. Create dialog and show non-blocking.
    OptimizerDialog *d = new OptimizerDialog(nullptr);
    d->setAttribute(Qt::WA_DeleteOnClose);
    d->show();
}

extern "C" void open_model_downloads_cb(void *private_data)
{
    // Open a helpful search for RVC models (Hugging Face search)
    QDesktopServices::openUrl(QUrl("https://huggingface.co/models?search=rvc"));
}

extern "C" void open_docs_cb(void *private_data)
{
    // Open the local README.md (project docs) if available; otherwise open repo page
    // Prefer file URL to workspace README
    QString localReadme = QCoreApplication::applicationDirPath() + "/../README.md";
    QUrl fileUrl = QUrl::fromLocalFile(localReadme);
    if (!QDesktopServices::openUrl(fileUrl))
        QDesktopServices::openUrl(QUrl("https://github.com/myrqyry/live-latent-lab-sound"));
}

extern "C" void check_updates_cb(void *private_data)
{
    // Open the project's releases or documentation page where updates are posted
    QDesktopServices::openUrl(QUrl("https://github.com/myrqyry/live-latent-lab-sound/releases"));
}

void init_optimizer_tools_menu()
{
    // Register a Tools submenu "Live Latent Lab Tools" with several entries.
    obs_frontend_add_tools_menu_item("Live Latent Lab Tools/RVC Model Optimizer", optimizer_tools_cb, nullptr);
    obs_frontend_add_tools_menu_item("Live Latent Lab Tools/Download Models", open_model_downloads_cb, nullptr);
    obs_frontend_add_tools_menu_item("Live Latent Lab Tools/Project Docs", open_docs_cb, nullptr);
    obs_frontend_add_tools_menu_item("Live Latent Lab Tools/Check For Updates", check_updates_cb, nullptr);
}

#include "optimizer_ui.moc"

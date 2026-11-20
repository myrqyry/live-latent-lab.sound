#include <QString>
#include <obs.h>

void log_qt_version() {
    QString version_msg = QString("Qt Version verified: %1").arg(QT_VERSION_STR);
    blog(LOG_INFO, "%s", version_msg.toUtf8().constData());
}

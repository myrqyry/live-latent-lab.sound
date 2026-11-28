// Real module property defaults, using a disposable libobs config directory.
#include <obs.h>
#include <obs-module.h>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QSettings>
#include <QString>
#include <cstdio>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"failed: %s\n",#x);return 1;}}while(false)
int main(int argc,char**argv){
 QCoreApplication app(argc,argv);
 CHECK(argc==3);
 QTemporaryDir tmp;CHECK(tmp.isValid());
 CHECK(QDir(tmp.path()).mkdir("models"));
 CHECK(QDir(tmp.path()).mkdir("runtime"));
 CHECK(obs_startup("en-US",tmp.path().toUtf8().constData(),nullptr));
 obs_module_t*m=nullptr;CHECK(obs_open_module(&m,argv[1],argv[2])==MODULE_SUCCESS&&obs_init_module(m));
 char*path=obs_module_get_config_path(m,"preferences.ini");CHECK(path);
 {QSettings s(QString::fromUtf8(path),QSettings::IniFormat);s.setValue("models",tmp.filePath("models"));s.setValue("runtimes",tmp.filePath("runtime"));s.sync();CHECK(s.status()==QSettings::NoError);}
 bfree(path);
 auto*p=obs_get_source_properties("yamnet_classifier_source");CHECK(p);
 CHECK(QString::fromUtf8(obs_property_path_default_path(obs_properties_get(p,"yamnet_model")))==tmp.filePath("models"));
 CHECK(QString::fromUtf8(obs_property_path_default_path(obs_properties_get(p,"mediapipe_library")))==tmp.filePath("runtime"));
 obs_properties_destroy(p);
 // A legacy QSettings value containing a comma is quoted in the INI.
 CHECK(QDir(tmp.path()).mkdir("models, test"));
 path=obs_module_get_config_path(m,"preferences.ini");CHECK(path);
 {QSettings s(QString::fromUtf8(path),QSettings::IniFormat);s.setValue("models",tmp.filePath("models, test"));s.sync();CHECK(s.status()==QSettings::NoError);}
 bfree(path);
 p=obs_get_source_properties("yamnet_classifier_source");CHECK(p);
 CHECK(QString::fromUtf8(obs_property_path_default_path(obs_properties_get(p,"yamnet_model")))==tmp.filePath("models, test"));
 obs_properties_destroy(p);
 auto*defaults=obs_get_source_defaults("yamnet_classifier_source");
 CHECK(defaults&&QString::fromUtf8(obs_data_get_string(defaults,"yamnet_model")).isEmpty());
 obs_data_release(defaults);obs_shutdown();
 puts("SOUND_SETTINGS_NATIVE_PICKER_PASS=1");
}

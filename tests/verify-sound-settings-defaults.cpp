// Exercise the real settings page registered by the actual staged OBS module.
// The dialog shell supplies the same named controls as native OBS Settings.
#include <obs.h>
#include <obs-module.h>
#include <util/bmem.h>
#include <util/config-file.h>
#include <QApplication>
#include <QDialog>
#include <QListWidget>
#include <QStackedWidget>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QTabWidget>
#include <QLineEdit>
#include <QLabel>
#include <QTemporaryDir>
#include <QDir>
#include <QFileInfo>
#include <QVBoxLayout>
#include <cstdio>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"failed: %s\n",#x);return 1;}}while(false)
int main(int argc,char**argv){
 QApplication app(argc,argv);
 CHECK(argc==3);
 QTemporaryDir tmp;CHECK(tmp.isValid());
 CHECK(obs_startup("en-US",tmp.path().toUtf8().constData(),nullptr));
 QDialog dialog;dialog.setObjectName("OBSBasicSettings");
 auto*layout=new QVBoxLayout(&dialog);
 auto*nav=new QListWidget(&dialog);nav->setObjectName("listWidget");
 auto*stack=new QStackedWidget(&dialog);stack->setObjectName("settingsPages");
 auto*buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel|QDialogButtonBox::Apply,&dialog);
 buttons->setObjectName("buttonBox");
 layout->addWidget(nav);layout->addWidget(stack);layout->addWidget(buttons);
 for(int i=0;i<9;++i){nav->addItem(QString::number(i));stack->addWidget(new QWidget);}
 QObject::connect(nav,&QListWidget::currentRowChanged,stack,&QStackedWidget::setCurrentIndex);
 auto*apply=buttons->button(QDialogButtonBox::Apply);apply->setEnabled(false);
 QObject::connect(buttons,&QDialogButtonBox::clicked,&dialog,[&](QAbstractButton*b){
     if(buttons->buttonRole(b)==QDialogButtonBox::ApplyRole)apply->setEnabled(false);
 });
 obs_module_t*m=nullptr;CHECK(obs_open_module(&m,argv[1],argv[2])==MODULE_SUCCESS&&obs_init_module(m));
 obs_post_load_modules();
 auto*tabs=dialog.findChild<QTabWidget*>("plugin-settings-tabs-v1");CHECK(tabs&&tabs->count()==1);
 auto*page=tabs->widget(0);
 auto*models=page->findChild<QLineEdit*>("models"),*runtimes=page->findChild<QLineEdit*>("runtimes");
 CHECK(models&&runtimes);
 char*config=obs_module_get_config_path(m,"preferences.ini");CHECK(config);
 QString pref=QString::fromUtf8(config);bfree(config);
 auto root=QFileInfo(pref).absolutePath();
 auto default_models=root+"/models",default_runtimes=root+"/runtimes";
 CHECK(models->text()==default_models&&runtimes->text()==default_runtimes);
 CHECK(QFileInfo(default_models).isDir()&&QFileInfo(default_runtimes).isDir());
 CHECK(!QFileInfo::exists(pref)&&!apply->isEnabled());
 CHECK(!models->isReadOnly()&&!runtimes->isReadOnly());
 auto picker=[&](const char*key){
     auto*p=obs_get_source_properties("yamnet_classifier_source");
     QString result=QString::fromUtf8(obs_property_path_default_path(obs_properties_get(p,key)));
     obs_properties_destroy(p);return result;
 };
 CHECK(picker("yamnet_model")==default_models&&picker("mediapipe_library")==default_runtimes);
 auto custom_models=tmp.filePath("my models, test"),custom_runtimes=tmp.filePath("my runtimes");
 CHECK(QDir().mkpath(custom_models)&&QDir().mkpath(custom_runtimes));
 models->setText(custom_models);runtimes->setText(custom_runtimes);
 CHECK(apply->isEnabled()&&!QFileInfo::exists(pref));
 apply->click();CHECK(!apply->isEnabled());
 CHECK(picker("yamnet_model")==custom_models&&picker("mediapipe_library")==custom_runtimes);
 models->setText(tmp.filePath("missing"));apply->click();
 CHECK(apply->isEnabled()&&picker("yamnet_model")==custom_models);
 auto*reset_models=page->findChild<QPushButton*>("reset-models");
 auto*reset_runtimes=page->findChild<QPushButton*>("reset-runtimes");
 CHECK(reset_models&&reset_runtimes);
 reset_models->click();reset_runtimes->click();
 CHECK(models->text()==default_models&&runtimes->text()==default_runtimes);
 CHECK(picker("yamnet_model")==custom_models);
 apply->click();CHECK(!apply->isEnabled());
 CHECK(picker("yamnet_model")==default_models&&picker("mediapipe_library")==default_runtimes);
 config_t*c=nullptr;CHECK(config_open(&c,pref.toUtf8().constData(),CONFIG_OPEN_EXISTING)==CONFIG_SUCCESS);
 CHECK(QString::fromUtf8(config_get_string(c,"General","models")).isEmpty());
 CHECK(QString::fromUtf8(config_get_string(c,"General","runtimes")).isEmpty());config_close(c);
 obs_shutdown();
 puts("SOUND_SETTINGS_DEFAULTS_PASS=1");
}

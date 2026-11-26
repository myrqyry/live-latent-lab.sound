#include "../src/ui/plugin-settings-page.hpp"
#include <QApplication>
#include <QDialog>
#include <QListWidget>
#include <QStackedWidget>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QLineEdit>
#include <QVBoxLayout>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QSettings>
#include <cstdio>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"failed: %s\n",#x);return 1;}}while(false)
int main(int argc,char**argv){
 QApplication app(argc,argv);
 QTemporaryDir tmp;CHECK(tmp.isValid());
 QDialog dialog;dialog.setObjectName("OBSBasicSettings");
 auto*layout=new QVBoxLayout(&dialog);
 auto*nav=new QListWidget(&dialog);nav->setObjectName("listWidget");
 auto*stack=new QStackedWidget(&dialog);stack->setObjectName("settingsPages");
 auto*buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel|QDialogButtonBox::Apply,&dialog);buttons->setObjectName("buttonBox");
 layout->addWidget(nav);layout->addWidget(stack);layout->addWidget(buttons);
 for(int i=0;i<9;++i){nav->addItem(QString::number(i));stack->addWidget(new QWidget);}
 QObject::connect(nav,&QListWidget::currentRowChanged,stack,&QStackedWidget::setCurrentIndex);
 auto*apply=buttons->button(QDialogButtonBox::Apply);apply->setEnabled(false);
 // Emulate native OBS's existing button handler: successful Apply/OK clears Apply.
 bool accept=true;
 QObject::connect(buttons,&QDialogButtonBox::clicked,&dialog,[&](QAbstractButton*b){if(accept&&(buttons->buttonRole(b)==QDialogButtonBox::ApplyRole||buttons->buttonRole(b)==QDialogButtonBox::AcceptRole))apply->setEnabled(false);});
 int saved=0;
 auto make=[&](QWidget*parent){auto*edit=new QLineEdit(parent);edit->setObjectName("draft");return edit;};
 auto save=[&](QWidget*p){QSettings s(tmp.filePath("prefs.ini"),QSettings::IniFormat);s.setValue("model",static_cast<QLineEdit*>(p)->text());s.sync();++saved;return s.status()==QSettings::NoError;};
 plugin_settings::Section first("sound","Sound",make,save);
 plugin_settings::Section second("sight","Sight",make,[](QWidget*){return true;});
 first.scan();second.scan();first.scan();
 CHECK(nav->count()==10&&stack->count()==10);
 auto*tabs=dialog.findChild<QTabWidget*>("plugin-settings-tabs-v1");CHECK(tabs&&tabs->count()==2);
 auto*edit=qobject_cast<QLineEdit*>(tabs->widget(0));CHECK(edit);
 edit->setText("/models");first.markDirty(edit);CHECK(apply->isEnabled());
 accept=false;apply->click();CHECK(saved==0);
 accept=true;apply->click();CHECK(saved==1);
 QSettings s(tmp.filePath("prefs.ini"),QSettings::IniFormat);CHECK(s.value("model")=="/models");
 edit->setText("/discard");first.markDirty(edit);buttons->button(QDialogButtonBox::Cancel)->click();CHECK(saved==1);
 first.detach();CHECK(tabs->count()==1&&nav->count()==10);
 second.detach();CHECK(nav->count()==9&&stack->count()==9);
 first.scan();CHECK(nav->count()==10);first.detach();CHECK(nav->count()==9);
 stack->addWidget(new QWidget);first.scan();CHECK(nav->count()==9);
 puts("SOUND_SHARED_SETTINGS_PAGE_PASS=1");
}

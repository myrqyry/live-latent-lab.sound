#include "plugin-settings-page.hpp"
#include "../framework/preferences.hpp"
#include <obs-module.h>
#include <util/bmem.h>
#include <QSettings>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QScrollArea>
#include <QDesktopServices>
#include <QUrl>
#include <QFileInfo>

namespace {
std::unique_ptr<plugin_settings::Section> section;
QString configPath(){
    char*p=obs_module_config_path("preferences.ini");
    if(!p)return {};
    auto path=QString::fromUtf8(p);bfree(p);return path;
}
QString pref(const char*key){
    auto path=configPath();if(path.isEmpty())return {};
    return QSettings(path,QSettings::IniFormat).value(key).toString();
}
void link(QVBoxLayout*layout,QWidget*parent,const QString&title,const QString&url){
    auto*label=new QLabel(parent);label->setTextFormat(Qt::RichText);
    label->setText("<a href=\""+url.toHtmlEscaped()+"\">"+title.toHtmlEscaped()+"</a>");
    label->setTextInteractionFlags(Qt::TextBrowserInteraction);
    QObject::connect(label,&QLabel::linkActivated,label,[url](const QString&){QDesktopServices::openUrl(QUrl(url));});
    layout->addWidget(label);
}
const QString docs="https://github.com/myrqyry/live-latent-lab.sound/blob/main/docs/";
QWidget* makePage(QWidget*parent){
    auto*page=new QWidget(parent);
    auto*root=new QVBoxLayout(page);root->setContentsMargins(8,6,8,6);
    auto*scroll=new QScrollArea(page);scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);root->addWidget(scroll);
    auto*body=new QWidget(scroll);auto*layout=new QVBoxLayout(body);scroll->setWidget(body);
    auto*heading=new QLabel("Sound · Models and runtimes",body);auto font=heading->font();font.setBold(true);heading->setFont(font);layout->addWidget(heading);
    auto*hint=new QLabel("Choose starting folders for model and runtime file pickers. Existing sources keep their own settings. Downloads open in your browser.",body);
    hint->setWordWrap(true);layout->addWidget(hint);
    auto*form=new QFormLayout;layout->addLayout(form);
    auto picker=[&](const char*key,const QString&title){
        auto*row=new QWidget(body);auto*h=new QHBoxLayout(row);h->setContentsMargins(0,0,0,0);
        auto*edit=new QLineEdit(pref(key),row);edit->setObjectName(key);edit->setReadOnly(true);edit->setPlaceholderText("System default");
        auto*browse=new QPushButton("Browse…",row);auto*clear=new QPushButton("Clear",row);
        h->addWidget(edit,1);h->addWidget(browse);h->addWidget(clear);form->addRow(title,row);
        QObject::connect(browse,&QPushButton::clicked,row,[page,edit]{auto dir=QFileDialog::getExistingDirectory(page,"Choose folder",edit->text());if(!dir.isEmpty())edit->setText(dir);});
        QObject::connect(clear,&QPushButton::clicked,edit,&QLineEdit::clear);
        QObject::connect(edit,&QLineEdit::textChanged,page,[page]{if(section)section->markDirty(page);});
    };
    picker("models","Model library");picker("runtimes","Runtime library");
    auto*note=new QLabel("Changes apply to newly opened file pickers. Use OBS Apply or OK to save.",body);note->setWordWrap(true);note->setObjectName("sound-preferences-status");layout->addWidget(note);
#ifdef SOUND_WITH_YAMNET
    link(layout,body,"YAMNet · verified setup",docs+"yamnet-setup.md");
    link(layout,body,"YAMNet float32 model (Google)","https://storage.googleapis.com/mediapipe-models/audio_classifier/yamnet/float32/1/yamnet.tflite");
    link(layout,body,"MediaPipe 1.2.3 runtime (PyPI)","https://pypi.org/project/mediapipe/1.2.3/");
#endif
#ifdef SOUND_WITH_STT
    link(layout,body,"Speech recognition · Whisper / Parakeet / Nemotron setup",docs+"expanded-build.md");
    link(layout,body,"Zipformer model (Hugging Face)","https://huggingface.co/csukuangfj/sherpa-onnx-streaming-zipformer-en-20M-2023-02-17/tree/d42f2d9f7ca24806fb667456a18a9f1b60f70d16");
#endif
#ifdef SOUND_WITH_TTS
    link(layout,body,"Speech synthesis · worker setup",docs+"expanded-build.md");
    link(layout,body,"Native Kokoro · model/runtime setup",docs+"native-kokoro.md");
    link(layout,body,"Native Chatterbox Turbo · model/runtime setup",docs+"native-turbo.md");
#endif
#ifdef SOUND_WITH_STEM_LIVE
    link(layout,body,"HTDemucs · worker and model setup",docs+"expanded-build.md");
#endif
#ifdef SOUND_WITH_DIFFUSION
    link(layout,body,"Stable Audio 3 / SAME-S · model and runtime setup",docs+"audio-diffusion.md");
#endif
#ifdef SOUND_WITH_DDSP
    link(layout,body,"DDSP · model pack and LiteRT setup",docs+"ddsp.md");
    link(layout,body,"DDSP model pack (Google)","https://storage.googleapis.com/ddsp-vst/releases/DDSP-VST-Models.zip");
#endif
#ifdef SOUND_WITH_RVC
    link(layout,body,"RVC · local checkpoints and runtime setup",docs+"native-rvc.md");
    link(layout,body,"RVC GGUF models (Hugging Face)","https://huggingface.co/audio-cpp/audio.cpp-gguf/tree/46877f63ff8a2e4704b4925176a40e9b3e9d456e/RVC-GGUF");
#endif
    link(layout,body,"Model and runtime licensing / distribution",docs+"distribution.md");
    layout->addStretch();return page;
}
bool savePage(QWidget*page){
    auto path=configPath();if(path.isEmpty())return false;
    QSettings settings(path,QSettings::IniFormat);
    for(auto key:{"models","runtimes"})settings.setValue(key,page->findChild<QLineEdit*>(key)->text());
    settings.sync();
    bool ok=settings.status()==QSettings::NoError;
    page->findChild<QLabel*>("sound-preferences-status")->setText(ok?"Sound preferences saved.":"Could not save Sound preferences.");
    return ok;
}
}
namespace sound_preferences {
std::string directory(bool runtime){
    auto dir=pref(runtime?"runtimes":"models");
    return QFileInfo(dir).isDir()&&!dir.isEmpty()?QDir::cleanPath(dir).toUtf8().toStdString():std::string{};
}
}
void register_sound_settings(){
    if(section||!qobject_cast<QApplication*>(QCoreApplication::instance()))return;
    section=std::make_unique<plugin_settings::Section>("lll.sound","LLL.sound",makePage,savePage);
    section->scan();
    blog(LOG_INFO,"[Sound] Native plugin settings bridge registered");
}
void unregister_sound_settings(){section.reset();}

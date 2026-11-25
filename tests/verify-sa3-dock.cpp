#include "ui/diffusion_panel.hpp"
#include <obs-module.h>
#include <QApplication>
#include <QTest>
#include <QElapsedTimer>
#include <cstdio>
#include <functional>
static bool waitFor(std::function<bool()> predicate,int ms){
    QElapsedTimer t;t.start();
    while(t.elapsed()<ms){if(predicate())return true;QTest::qWait(25);}
    return predicate();
}
static int64_t counter(obs_source_t* s,const char* key){
    calldata_t d{};bool ok=proc_handler_call(obs_source_get_proc_handler(s),"sound_diffusion_status_v3",&d);
    auto result=ok?calldata_int(&d,key):-1;calldata_free(&d);return result;
}
static QString promptOf(obs_source_t* s){
    auto* d=obs_source_get_settings(s);auto value=QString::fromUtf8(obs_data_get_string(d,"prompt"));obs_data_release(d);return value;
}
int main(int argc,char** argv){
    QApplication app(argc,argv);
    if(argc!=4||!obs_startup("en-US",nullptr,nullptr))return 2;
    obs_audio_info ai{};ai.samples_per_sec=48000;ai.speakers=SPEAKERS_STEREO;
    if(!obs_reset_audio(&ai))return 3;
    obs_video_info v{};v.graphics_module="libobs-opengl";v.fps_num=30;v.fps_den=1;
    v.base_width=v.output_width=320;v.base_height=v.output_height=240;v.output_format=VIDEO_FORMAT_RGBA;
    v.colorspace=VIDEO_CS_DEFAULT;v.range=VIDEO_RANGE_DEFAULT;v.scale_type=OBS_SCALE_DISABLE;
    if(obs_reset_video(&v)!=OBS_VIDEO_SUCCESS)return 3;
    obs_module_t* m=nullptr;
    if(obs_open_module(&m,argv[1],argv[2])!=MODULE_SUCCESS||!obs_init_module(m))return 4;
    auto* d=obs_data_create();obs_data_set_string(d,"models_dir",argv[3]);obs_data_set_string(d,"prompt","mellow piano");
    obs_data_set_double(d,"duration",2);obs_data_set_int(d,"seed",42);
    auto* a=obs_source_create("sound_audio_diffusion_source","Dock A",d,nullptr);
    obs_data_release(d);d=obs_data_create();obs_data_set_string(d,"models_dir",argv[3]);obs_data_set_string(d,"prompt","soft drums");
    auto* b=obs_source_create("sound_audio_diffusion_source","Dock B",d,nullptr);obs_data_release(d);
    if(!a||!b)return 5;
    obs_set_output_source(1,a);obs_source_set_audio_mixers(a,1);obs_source_set_audio_active(a,true);
    bool ok=false;
    {
        DiffusionPanel panel;panel.resize(320,660);panel.show();
        auto* picker=panel.findChild<QComboBox*>("diffusion_source");
        auto* prompt=panel.findChild<QPlainTextEdit*>("diffusion_prompt");
        auto* apply=panel.findChild<QPushButton*>("diffusion_apply");
        auto* generate=panel.findChild<QPushButton*>("diffusion_generate");
        auto* stop=panel.findChild<QPushButton*>("diffusion_stop");
        auto* telemetry=panel.findChild<QLabel*>("diffusion_telemetry");
        if(!picker||!prompt||!apply||!generate||!stop||!telemetry)return 6;
        bool discovered=waitFor([&]{return picker->findData(QString::fromUtf8(obs_source_get_uuid(a)))>=0&&picker->findData(QString::fromUtf8(obs_source_get_uuid(b)))>=0;},2000);
        picker->setCurrentIndex(picker->findData(QString::fromUtf8(obs_source_get_uuid(a))));
        bool selected=waitFor([&]{return prompt->toPlainText()=="mellow piano"&&generate->isEnabled();},2000);
        prompt->setPlainText("instrumental soft toy keyboard and bass");
        QTest::qWait(600);
        bool draft=promptOf(a)=="mellow piano"&&prompt->toPlainText().contains("toy keyboard")&&apply->isEnabled();
        apply->click();
        bool saved=waitFor([&]{return promptOf(a).contains("toy keyboard")&&promptOf(b)=="soft drums";},2000);
        printf("DOCK_PRE_GENERATE discovered=%d selected=%d draft=%d saved=%d active=%d enabled=%d prompt=%s\n",discovered,selected,draft,saved,obs_source_active(a),generate->isEnabled(),promptOf(a).toUtf8().constData());fflush(stdout);
        generate->click();
        bool generated=waitFor([&]{return counter(a,"played")>=1;},20000);
        bool live=waitFor([&]{return telemetry->text().contains("2.0 s")&&telemetry->text().contains("0 underruns");},2000);
        stop->click();
        auto* revert=panel.findChild<QPushButton*>("diffusion_revert");
        prompt->setPlainText("draft not to be saved");
        auto* external=obs_data_create();obs_data_set_string(external,"prompt","external change");obs_source_update(a,external);obs_data_release(external);
        apply->click();
        bool conflict=promptOf(a)=="external change"&&prompt->toPlainText()=="draft not to be saved";
        picker->setCurrentIndex(picker->findData(QString::fromUtf8(obs_source_get_uuid(b))));
        bool blocked=picker->currentData().toString()==QString::fromUtf8(obs_source_get_uuid(a));
        revert->click();
        conflict=conflict&&prompt->toPlainText()=="external change";
        picker->setCurrentIndex(picker->findData(QString::fromUtf8(obs_source_get_uuid(b))));
        bool isolated=waitFor([&]{return prompt->toPlainText()=="soft drums"&&!generate->isEnabled();},2000);
        obs_source_remove(b);obs_source_release(b);b=nullptr;
        bool removed=waitFor([&]{return picker->currentData().toString().isEmpty()&&!generate->isEnabled();},2000);
        ok=discovered&&selected&&draft&&saved&&generated&&live&&isolated&&removed&&conflict&&blocked;
        printf("SA3_DOCK_DRAFT_CONFLICT=%d SWITCH_BLOCKED=%d\n",conflict,blocked);
        printf("SA3_DOCK discovered=%d selected=%d draft=%d saved=%d generated=%d live=%d isolated=%d removed=%d pass=%d\n",discovered,selected,draft,saved,generated,live,isolated,removed,ok);
    }
    obs_set_output_source(1,nullptr);obs_source_release(a);if(b)obs_source_release(b);obs_wait_for_destroy_queue();obs_shutdown();return ok?0:1;
}

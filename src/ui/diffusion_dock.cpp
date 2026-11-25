#include "diffusion_panel.hpp"
#include <obs-frontend-api.h>
#include <QPointer>
#include <QApplication>

namespace {
constexpr const char* dock_id="LLLSoundAudioDiffusion";
QPointer<DiffusionPanel> panel;
bool registered=false;
void remove_dock(){
    if(!panel)return;
    auto old=panel;
    panel->hide();
    obs_frontend_remove_dock(dock_id);
    if(old)delete old.data();
    panel.clear();
}
void event(obs_frontend_event e,void*){
    if(e==OBS_FRONTEND_EVENT_FINISHED_LOADING&&!panel){
        if(!qobject_cast<QApplication*>(QCoreApplication::instance())||!obs_frontend_get_main_window())return;
        auto* widget=new DiffusionPanel;
        if(!obs_frontend_add_dock_by_id(dock_id,"LLL.sound Audio Diffusion",widget)){
            delete widget;
            blog(LOG_WARNING,"[Sound/Diffusion] Dock registration failed");return;
        }
        panel=widget;
        blog(LOG_INFO,"[Sound/Diffusion] Audio Diffusion dock registered");
    }else if(e==OBS_FRONTEND_EVENT_EXIT){
        remove_dock();
        if(registered){obs_frontend_remove_event_callback(event,nullptr);registered=false;}
    }
}
}
void register_diffusion_dock(){
    if(registered||!qobject_cast<QApplication*>(QCoreApplication::instance()))return;
    obs_frontend_add_event_callback(event,nullptr);registered=true;
}
void unregister_diffusion_dock(){
    if(registered){obs_frontend_remove_event_callback(event,nullptr);registered=false;}
    remove_dock();
}

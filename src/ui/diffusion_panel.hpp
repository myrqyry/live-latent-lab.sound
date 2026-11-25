#pragma once
#include <obs.h>
#include <QWidget>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <cmath>
#include <QPushButton>
#include <QLabel>
#include <QProgressBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTimer>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QHideEvent>
#include <QScrollArea>
#include <QFileDialog>
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

class DiffusionPanel final : public QWidget {
    struct Release {void operator()(obs_source_t* p)const{if(p)obs_source_release(p);}};
    using Source=std::unique_ptr<obs_source_t,Release>;
    struct Settings {
        QString prompt;
        double duration=12;int steps=8;
        bool operator==(const Settings& b)const{return prompt==b.prompt&&std::abs(duration-b.duration)<.005&&steps==b.steps;}
    };
    QComboBox* picker_=nullptr;
    QPlainTextEdit* prompt_=nullptr;
    QDoubleSpinBox* duration_=nullptr;QSpinBox* steps_=nullptr;
    QPushButton *apply_=nullptr,*revert_=nullptr,*generate_=nullptr,*continuous_=nullptr,*finish_=nullptr,*stop_=nullptr,*replay_=nullptr,*models_=nullptr;
    QLabel *message_=nullptr,*telemetry_=nullptr,*notice_=nullptr;
    QProgressBar* progress_=nullptr;
    QTimer* timer_=nullptr;
    QString selected_;
    Settings baseline_;
    bool dirty_=false,loading_=false;
    static bool suitable(obs_source_t* s){
        const auto* id=s?obs_source_get_unversioned_id(s):nullptr;
        return id&&!std::strcmp(id,"sound_audio_diffusion_source")&&!obs_source_removed(s);
    }
    Source source()const{
        Source p(selected_.isEmpty()?nullptr:obs_get_source_by_uuid(selected_.toUtf8().constData()));
        if(!suitable(p.get()))p.reset();
        return p;
    }
    static Settings read(obs_source_t* s){
        auto* d=obs_source_get_settings(s);
        auto duration=obs_data_get_double(d,"duration");
        if(!std::isfinite(duration))duration=12.;
        Settings a{QString::fromUtf8(obs_data_get_string(d,"prompt")),std::clamp(duration,1.,60.),int(std::clamp<long long>(obs_data_get_int(d,"steps"),1,32))};
        // Preserve fractional duration unless the user actually changes it.
        obs_data_release(d);return a;
    }
    Settings draft()const{return {prompt_->toPlainText(),duration_->value(),steps_->value()};}
    void load(const Settings& a){
        loading_=true;baseline_=a;prompt_->setPlainText(a.prompt);duration_->setValue(a.duration);steps_->setValue(a.steps);
        dirty_=false;loading_=false;notice_->clear();
    }
    void changed(){
        if(loading_)return;
        dirty_=!(draft()==baseline_);
        notice_->setText(dirty_?"Unapplied changes":"");
        refreshStatus();
    }
    void refreshSources(){
        std::vector<std::pair<QString,QString>> items;
        obs_enum_sources([](void* p,obs_source_t* s){
            if(suitable(s)&&obs_source_get_uuid(s))
                static_cast<std::vector<std::pair<QString,QString>>*>(p)->emplace_back(QString::fromUtf8(obs_source_get_name(s)),QString::fromUtf8(obs_source_get_uuid(s)));
            return true;
        },&items);
        std::sort(items.begin(),items.end());
        bool same=picker_->count()==int(items.size()+1);
        for(size_t i=0;same&&i<items.size();++i)same=picker_->itemText(int(i+1))==items[i].first&&picker_->itemData(int(i+1)).toString()==items[i].second;
        if(same)return;
        QSignalBlocker blocker(picker_);
        picker_->clear();picker_->addItem("Select audio diffusion source",QString());
        for(const auto& item:items)picker_->addItem(item.first,item.second);
        const auto index=picker_->findData(selected_);
        if(index<0){selected_.clear();load({});notice_->setText("Source removed");}
        picker_->setCurrentIndex(std::max(0,index));
    }
    void choose(){
        const auto next=picker_->currentData().toString();
        if(next==selected_)return;
        if(dirty_&&source()){
            QSignalBlocker blocker(picker_);picker_->setCurrentIndex(picker_->findData(selected_));
            notice_->setText("Apply or revert changes before switching source");return;
        }
        selected_=next;auto s=source();load(s?read(s.get()):Settings{});refreshStatus();
    }
    void apply(){
        auto s=source();if(!s)return;
        const auto a=draft(),live=read(s.get());
        if((a.prompt!=baseline_.prompt&&live.prompt!=baseline_.prompt)||
           (std::abs(a.duration-baseline_.duration)>=.005&&std::abs(live.duration-baseline_.duration)>=.005)||
           (a.steps!=baseline_.steps&&live.steps!=baseline_.steps)){
            notice_->setText("Source changed elsewhere. Revert to reload.");return;
        }
        auto* d=obs_data_create();
        if(a.prompt!=baseline_.prompt)obs_data_set_string(d,"prompt",a.prompt.toUtf8().constData());
        if(std::abs(a.duration-baseline_.duration)>=.005)obs_data_set_double(d,"duration",a.duration);
        if(a.steps!=baseline_.steps)obs_data_set_int(d,"steps",a.steps);
        obs_source_update(s.get(),d);obs_data_release(d);load(read(s.get()));refreshStatus();
    }
    void command(const char* key){
        auto s=source();if(!s)return;
        const bool emergency=!std::strcmp(key,"stop")||!std::strcmp(key,"finish_segment");
        if(!emergency&&(dirty_||!obs_source_active(s.get())||!obs_source_enabled(s.get())))return;
        auto* props=obs_source_properties(s.get());
        if(props){if(auto* p=obs_properties_get(props,key))obs_property_button_clicked(p,s.get());obs_properties_destroy(props);}
        refreshStatus();
    }
    void refreshStatus(){
        auto s=source();
        bool active=s&&obs_source_active(s.get())&&obs_source_enabled(s.get());
        bool continuous=false,called=false;
        if(s){
            if(!dirty_){const auto current=read(s.get());if(!(current==baseline_))load(current);}
            calldata_t d{};called=proc_handler_call(obs_source_get_proc_handler(s.get()),"sound_diffusion_status_v3",&d);
            if(called){
                continuous=calldata_bool(&d,"continuous");
                auto* msg=calldata_string(&d,"message");
                auto* stage=calldata_string(&d,"stage");
                message_->setText(QString::fromUtf8(msg?msg:"Ready")+QStringLiteral(" · ")+QString::fromUtf8(stage?stage:"idle"));
                progress_->setValue(int(std::clamp<long long>(calldata_int(&d,"progress"),0,100)));
                telemetry_->setText(QString("%1 s played · %2 underruns\nBuffer %3 s · Next %4\nLast generation %5 ms")
                    .arg(calldata_int(&d,"played_ms")/1000.,0,'f',1).arg(calldata_int(&d,"underruns"))
                    .arg(calldata_int(&d,"buffer_ms")/1000.,0,'f',1)
                    .arg(calldata_bool(&d,"next_ready")?QString("%1 s ready").arg(calldata_int(&d,"next_ms")/1000.,0,'f',1):QString("not ready"))
                    .arg(calldata_int(&d,"generation_ms")));
            }
            calldata_free(&d);
        }
        if(!called){message_->setText(s?"Status unavailable":"Select an Audio Diffusion source");progress_->setValue(0);telemetry_->clear();}
        else if(!active)message_->setText("Inactive or disabled · "+message_->text());
        prompt_->setEnabled(bool(s));duration_->setEnabled(bool(s));steps_->setEnabled(bool(s));models_->setEnabled(bool(s)&&!dirty_);
        apply_->setEnabled(bool(s)&&dirty_);revert_->setEnabled(bool(s)&&dirty_);
        generate_->setEnabled(active&&called&&!dirty_);continuous_->setEnabled(active&&called&&!dirty_&&!continuous);
        finish_->setEnabled(active&&continuous);stop_->setEnabled(bool(s)&&called);replay_->setEnabled(active&&called&&!dirty_);
    }
    void pickModels(){
        auto s=source();if(!s||dirty_)return;
        auto* d=obs_source_get_settings(s.get());auto path=QString::fromUtf8(obs_data_get_string(d,"models_dir"));obs_data_release(d);
        const auto chosen=QFileDialog::getExistingDirectory(this,"Stable Audio 3 model directory",path);
        if(chosen.isEmpty()||obs_source_removed(s.get())||selected_!=QString::fromUtf8(obs_source_get_uuid(s.get())))return;
        d=obs_data_create();obs_data_set_string(d,"models_dir",chosen.toUtf8().constData());obs_source_update(s.get(),d);obs_data_release(d);
        notice_->setText("Model directory updated");refreshStatus();
    }
public:
    explicit DiffusionPanel(QWidget* parent=nullptr):QWidget(parent){
        auto* outer=new QVBoxLayout(this);outer->setContentsMargins(0,0,0,0);
        auto* scroll=new QScrollArea(this);scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);outer->addWidget(scroll);
        auto* content=new QWidget;scroll->setWidget(content);
        auto* v=new QVBoxLayout(content);v->setContentsMargins(6,6,6,6);v->setSpacing(5);
        auto button=[&](QLayout* row,const char* name,const char* title,auto handler){
            auto* b=new QPushButton(title);b->setObjectName(name);row->addWidget(b);connect(b,&QPushButton::clicked,this,handler);return b;
        };
        picker_=new QComboBox;picker_->setObjectName("diffusion_source");v->addWidget(picker_);
        connect(picker_,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{choose();});
        message_=new QLabel;message_->setTextFormat(Qt::PlainText);message_->setWordWrap(true);v->addWidget(message_);
        progress_=new QProgressBar;progress_->setRange(0,100);v->addWidget(progress_);
        telemetry_=new QLabel;telemetry_->setObjectName("diffusion_telemetry");telemetry_->setWordWrap(true);v->addWidget(telemetry_);
        auto* transport=new QHBoxLayout;v->addLayout(transport);
        generate_=button(transport,"diffusion_generate","Generate",[this]{command("generate");});
        continuous_=button(transport,"diffusion_continuous","Continuous",[this]{command("continuous");});
        auto* endings=new QHBoxLayout;v->addLayout(endings);
        finish_=button(endings,"diffusion_finish","Finish segment",[this]{command("finish_segment");});
        stop_=button(endings,"diffusion_stop","Stop",[this]{command("stop");});
        replay_=button(endings,"diffusion_replay","Replay",[this]{command("replay");});
        v->addWidget(new QLabel("Prompt"));
        prompt_=new QPlainTextEdit;prompt_->setObjectName("diffusion_prompt");prompt_->setFixedHeight(86);v->addWidget(prompt_);
        auto* controls=new QHBoxLayout;v->addLayout(controls);
        duration_=new QDoubleSpinBox;duration_->setDecimals(2);duration_->setRange(1,60);duration_->setSuffix(" s");controls->addWidget(new QLabel("Duration"));controls->addWidget(duration_);
        steps_=new QSpinBox;steps_->setRange(1,32);controls->addWidget(new QLabel("Steps"));controls->addWidget(steps_);
        auto* edit=new QHBoxLayout;v->addLayout(edit);
        apply_=button(edit,"diffusion_apply","Apply",[this]{apply();});
        revert_=button(edit,"diffusion_revert","Revert",[this]{auto s=source();if(s)load(read(s.get()));refreshStatus();});
        models_=button(v,"diffusion_models","Model directory…",[this]{pickModels();});
        notice_=new QLabel;notice_->setWordWrap(true);v->addWidget(notice_);v->addStretch(1);
        connect(prompt_,&QPlainTextEdit::textChanged,this,[this]{changed();});
        connect(duration_,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this]{changed();});
        connect(steps_,qOverload<int>(&QSpinBox::valueChanged),this,[this]{changed();});
        timer_=new QTimer(this);timer_->setInterval(500);connect(timer_,&QTimer::timeout,this,[this]{refreshSources();refreshStatus();});
        refreshSources();refreshStatus();
    }
    void showEvent(QShowEvent* e)override{QWidget::showEvent(e);refreshSources();refreshStatus();timer_->start();}
    void hideEvent(QHideEvent* e)override{timer_->stop();QWidget::hideEvent(e);}
};

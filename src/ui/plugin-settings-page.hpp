#pragma once
// Shared, versioned Qt widget convention. No cross-module C++ ABI or dependency
// on another plugin. Based on Aftermarket's native settings attachment pattern.
#include <QApplication>
#include <QDialog>
#include <QListWidget>
#include <QStackedWidget>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QTabWidget>
#include <QPointer>
#include <QTimer>
#include <QEvent>
#include <QHash>
#include <QIcon>
#include <QStyle>
#include <functional>
#include <memory>

namespace plugin_settings {
class Section final : public QObject {
    struct Entry {
        QPointer<QDialog> dialog;
        QPointer<QListWidget> nav;
        QPointer<QStackedWidget> stack;
        QPointer<QTabWidget> tabs;
        QPointer<QWidget> page;
        QPointer<QPushButton> apply;
        bool dirty=false;
    };
    QString id_,title_;
    std::function<QWidget*(QWidget*)> make_;
    std::function<bool(QWidget*)> save_;
    QHash<QDialog*,std::shared_ptr<Entry>> entries_;
    bool scheduled_=false;
    static constexpr const char* groupName="plugin-settings-tabs-v1";
    static constexpr const char* groupId="plugin-settings-v1";
    void remove(QDialog*dialog){
        auto e=entries_.take(dialog);
        if(!e||!e->tabs||!e->page)return;
        const int tab=e->tabs->indexOf(e->page);
        if(tab>=0)e->tabs->removeTab(tab);
        delete e->page.data();
        if(e->tabs->count()!=0||!e->nav||!e->stack)return;
        int row=e->stack->indexOf(e->tabs);
        if(row<0||row>=e->nav->count()||e->nav->item(row)->data(Qt::UserRole).toString()!=groupId)return;
        // Preserve native page/row alignment, including later-added pages.
        e->nav->setCurrentRow(0);
        delete e->nav->takeItem(row);
        e->stack->removeWidget(e->tabs);
        delete e->tabs.data();
    }
    void attach(QDialog* dialog){
        auto*nav=dialog->findChild<QListWidget*>("listWidget");
        auto*stack=dialog->findChild<QStackedWidget*>("settingsPages");
        auto*buttons=dialog->findChild<QDialogButtonBox*>("buttonBox");
        if(!nav||!stack||!buttons||nav->count()<8||nav->count()!=stack->count())return;
        auto*apply=buttons->button(QDialogButtonBox::Apply);
        if(!apply||!buttons->button(QDialogButtonBox::Ok))return;
        auto*tabs=stack->findChild<QTabWidget*>(groupName,Qt::FindDirectChildrenOnly);
        if(tabs){
            int row=stack->indexOf(tabs);
            if(row<0||nav->item(row)->data(Qt::UserRole).toString()!=groupId)return;
            for(int i=0;i<tabs->count();++i)if(tabs->widget(i)->property("plugin-settings-id-v1").toString()==id_)return;
        }else{
            // Avoid a collision with an unrelated extension's identically named group.
            for(int i=0;i<nav->count();++i)if(nav->item(i)->data(Qt::UserRole).toString()==groupId)return;
            tabs=new QTabWidget(stack);tabs->setObjectName(groupName);
            stack->addWidget(tabs);
            auto*item=new QListWidgetItem("Plugins",nav);item->setData(Qt::UserRole,groupId);
            item->setIcon(QIcon::fromTheme("preferences-plugin",qApp->style()->standardIcon(QStyle::SP_FileDialogDetailedView)));
        }
        auto e=std::make_shared<Entry>();
        e->dialog=dialog;e->nav=nav;e->stack=stack;e->tabs=tabs;e->apply=apply;
        e->page=make_(tabs);
        e->page->setProperty("plugin-settings-id-v1",id_);
        tabs->addTab(e->page,title_);
        entries_.insert(dialog,e);
        QObject::connect(dialog,&QObject::destroyed,this,[this,dialog]{entries_.remove(dialog);});
        // Native OBS connects first. It clears Apply after successful validation;
        // never commit on a rejected native Apply/OK or on Cancel.
        QObject::connect(buttons,&QDialogButtonBox::clicked,e->page,[this,e,buttons](QAbstractButton*b){
            const auto role=buttons->buttonRole(b);
            if(!e->dirty||!e->page||!e->apply||e->apply->isEnabled())return;
            if(role!=QDialogButtonBox::ApplyRole&&role!=QDialogButtonBox::AcceptRole)return;
            if(save_(e->page))e->dirty=false;
            else if(e->apply)e->apply->setEnabled(true);
        });
    }
    bool eventFilter(QObject*object,QEvent*event)override{
        if(object->objectName()!="OBSBasicSettings")return false;
        if(event->type()==QEvent::Show||event->type()==QEvent::Polish){
            if(!scheduled_){scheduled_=true;QTimer::singleShot(0,this,[this]{scheduled_=false;scan();});}
        }else if(event->type()==QEvent::Hide){
            // Close/Cancel discards drafts. Defer until native button handlers finish.
            QPointer<QDialog> dialog=qobject_cast<QDialog*>(object);
            QTimer::singleShot(0,this,[this,dialog]{if(dialog&&!dialog->isVisible())remove(dialog);});
        }
        return false;
    }
public:
    Section(QString id,QString title,std::function<QWidget*(QWidget*)> make,std::function<bool(QWidget*)> save)
        :id_(std::move(id)),title_(std::move(title)),make_(std::move(make)),save_(std::move(save)){
        if(qApp)qApp->installEventFilter(this);
    }
    ~Section()override{if(qApp)qApp->removeEventFilter(this);detach();}
    void scan(){if(!qApp)return;for(auto*w:QApplication::allWidgets()){
        auto*d=qobject_cast<QDialog*>(w);if(d&&d->objectName()=="OBSBasicSettings"&&!entries_.contains(d))attach(d);
    }}
    void markDirty(QWidget*page){for(auto&e:entries_)if(e->page==page){e->dirty=true;if(e->apply)e->apply->setEnabled(true);}}
    void detach(){for(auto*d:entries_.keys())remove(d);}
};
}

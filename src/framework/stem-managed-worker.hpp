#pragma once
// Linux worker-thread-only private process. Never use a global socket or kill
// a process obtained from a file. Own the child PID until waitpid reaps it.
#include <chrono>
#include <thread>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
namespace stts {
class StemManagedWorker {
public:
    StemManagedWorker()=default;
    StemManagedWorker(const StemManagedWorker&)=delete;
    StemManagedWorker& operator=(const StemManagedWorker&)=delete;
    ~StemManagedWorker(){stop();}
    const std::string& path()const{return path_;}
    pid_t pid()const{return pid_;}
    std::string error()const{
        if(dir_.empty())return {};
        std::ifstream f(dir_+"/worker.log",std::ios::binary|std::ios::ate);
        if(!f)return {};
        auto size=f.tellg();if(size<0)return {};
        f.seekg(size>2048?size-std::streamoff(2048):std::streampos(0));
        std::string out((std::istreambuf_iterator<char>(f)),{});
        return out;
    }
    bool start(const std::string& script,const std::string& profile,
               const std::string& python,const std::string& model,const std::string& safetensors){
        std::error_code ec;
        if(script.empty()||!std::filesystem::is_regular_file(script,ec)||!make_endpoint())return false;
        std::vector<std::string> args{"python3","-E","-s","-B",script,"run",profile,
            "--socket",path_,"--parent-pid",std::to_string(getpid())};
        if(!python.empty()){args.push_back("--python");args.push_back(python);}
        if(!model.empty()){args.push_back("--model");args.push_back(model);}
        if(!safetensors.empty()){args.push_back("--safetensors-package");args.push_back(safetensors);}
        return spawn(args);
    }
    bool start_native(const std::string& helper,const std::string& model){
        std::error_code ec;
        if(helper.empty()||model.empty()||!std::filesystem::is_regular_file(helper,ec)||
           access(helper.c_str(),X_OK)||!make_endpoint())return false;
        std::vector<std::string> args{helper,"--model",model,"--socket",path_,
            "--parent-pid",std::to_string(getpid())};
        return spawn(args);
    }
private:
    bool make_endpoint(){
        if(pid_>0||!dir_.empty())return false;
        char temp[]="/tmp/lll-stem-worker-XXXXXX";
        auto* dir=mkdtemp(temp);if(!dir)return false;
        dir_=dir;path_=dir_+"/model.sock";return true;
    }
    bool spawn(std::vector<std::string>& args){
        std::vector<char*> argv;for(auto& a:args)argv.push_back(a.data());argv.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        if(posix_spawn_file_actions_init(&actions))return false;
        bool ok=!posix_spawn_file_actions_addopen(&actions,0,"/dev/null",O_RDONLY,0)&&
            !posix_spawn_file_actions_addopen(&actions,1,(dir_+"/worker.log").c_str(),O_WRONLY|O_CREAT|O_EXCL,0600)&&
            !posix_spawn_file_actions_adddup2(&actions,1,2);
        pid_t pid=0;
        if(ok)ok=posix_spawnp(&pid,argv[0],&actions,nullptr,argv.data(),environ)==0;
        posix_spawn_file_actions_destroy(&actions);
        if(ok)pid_=pid;
        return ok;
    }
public:
    bool alive(){
        if(pid_<=0)return false;
        pid_t r;do{r=waitpid(pid_,nullptr,WNOHANG);}while(r<0&&errno==EINTR);
        if(r==0)return true;
        // ECHILD also means we have no safe ownership of this PID.
        pid_=0;return false;
    }
    template<class Current,class Duration>
    bool wait_ready(Current current,Duration timeout){
        auto until=std::chrono::steady_clock::now()+timeout;
        while(current()&&std::chrono::steady_clock::now()<until&&alive()){
            struct stat s{};
            if(lstat(path_.c_str(),&s)==0&&S_ISSOCK(s.st_mode)&&s.st_uid==geteuid()&&!(s.st_mode&077))return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    }
    void stop(){
        if(alive()){
            kill(pid_,SIGTERM);
            for(int i=0;i<20&&alive();++i)std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if(alive()){
                kill(pid_,SIGKILL);
                pid_t r;do{r=waitpid(pid_,nullptr,0);}while(r<0&&errno==EINTR);
                pid_=0;
            }
        }
        if(!dir_.empty()){
            unlink(path_.c_str());unlink((dir_+"/worker.log").c_str());rmdir(dir_.c_str());
            dir_.clear();path_.clear();
        }
    }
private:
    pid_t pid_=0;
    std::string dir_,path_;
};
}

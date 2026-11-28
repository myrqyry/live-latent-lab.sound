#include "framework/stem-managed-worker.hpp"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <thread>
#include <sys/wait.h>
using namespace stts;
int main(int argc,char** argv){
    assert(argc==2);
    std::filesystem::path root=argv[1];std::filesystem::create_directories(root);
    auto script=root/"worker.py";
    std::ofstream(script) <<
        "import argparse,socket,os,time\n"
        "p=argparse.ArgumentParser();p.add_argument('action');p.add_argument('profile');p.add_argument('--socket');p.add_argument('--parent-pid');a=p.parse_args()\n"
        "s=socket.socket(socket.AF_UNIX);s.bind(a.socket);os.chmod(a.socket,0o600);s.listen()\n"
        "while True: time.sleep(.1)\n";
    std::string path;
    pid_t pid;
    {
        StemManagedWorker child;
        assert(child.start(script.string(),"melband","","",""));
        path=child.path();pid=child.pid();
        assert(child.wait_ready([]{return true;},std::chrono::seconds(3)));
        assert(child.alive());
        assert(kill(pid,0)==0);
    }
    assert(!std::filesystem::exists(std::filesystem::path(path).parent_path()));
    assert(waitpid(pid,nullptr,WNOHANG)==-1&&errno==ECHILD);
    {
        StemManagedWorker child;
        assert(child.start(script.string(),"demucs","","",""));
        path=child.path();
        assert(!child.wait_ready([]{return false;},std::chrono::seconds(3)));
    }
    assert(!std::filesystem::exists(std::filesystem::path(path).parent_path()));
    {
        StemManagedWorker child;
        assert(child.start(script.string(),"melband","","",""));
        path=child.path();pid=child.pid();
        assert(child.wait_ready([]{return true;},std::chrono::seconds(3)));
        assert(kill(pid,SIGKILL)==0);
        auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(child.alive()&&std::chrono::steady_clock::now()<end)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        assert(!child.alive());
    }
    assert(!std::filesystem::exists(std::filesystem::path(path).parent_path()));
    {
        StemManagedWorker child;
        assert(!child.start((root/"missing").string(),"melband","","",""));
    }
    puts("STEM_MANAGED_WORKER_PASS=1");
}

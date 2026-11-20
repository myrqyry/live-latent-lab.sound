#define _GNU_SOURCE
#include "audiocpp.h"
#include "meanvc2-protocol.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static volatile sig_atomic_t stopped=0;
static void on_signal(int sig) { (void)sig; stopped=1; }
static int exact(int fd, void *data, size_t len, int output) {
    char *p=(char*)data;
    while(len) {
        ssize_t n=output?send(fd,p,len,MSG_NOSIGNAL):recv(fd,p,len,0);
        if(n>0) {p+=n;len-=(size_t)n;continue;}
        if(n<0 && errno==EINTR) {if(stopped)return -1;continue;}
        return -1;
    }
    return 0;
}
static void check(audiocpp_status s,const char* where) {
    if(s!=AUDIOCPP_OK) {
        fprintf(stderr,"MVC2_ERROR %s: %s\n",where,audiocpp_last_error());
        exit(3);
    }
}
static float *load_f32(const char* path,size_t* count) {
    FILE *f=fopen(path,"rb");
    if(!f) {perror(path);return NULL;}
    if(fseek(f,0,SEEK_END)) {fclose(f);return NULL;}
    long bytes=ftell(f);
    if(bytes<=0 || bytes>16000L*120L*4L || bytes%4L || fseek(f,0,SEEK_SET)) {
        fprintf(stderr,"MVC2_ERROR invalid reference PCM size\n");fclose(f);return NULL;
    }
    float *p=(float*)malloc((size_t)bytes);
    if(!p || fread(p,1,(size_t)bytes,f)!=(size_t)bytes) {
        free(p);fclose(f);return NULL;
    }
    fclose(f);*count=(size_t)bytes/4;return p;
}
int main(int argc,char**argv) {
    if(argc!=4) {
        fprintf(stderr,"Usage: meanvc2-sidecar MODEL.gguf REFERENCE-16K-MONO.f32 /absolute/private/socket\n");
        return 2;
    }
    const char *socket_path=argv[3];
    if(socket_path[0]!='/' || strlen(socket_path)>=sizeof(((struct sockaddr_un*)0)->sun_path)) {
        fprintf(stderr,"MVC2_ERROR socket path must be absolute and short\n");return 2;
    }
    struct stat st;
    if(lstat(socket_path,&st)==0 || errno!=ENOENT) {
        fprintf(stderr,"MVC2_ERROR socket path already occupied/inaccessible\n");return 2;
    }
    size_t ref_frames=0;float *ref=load_f32(argv[2],&ref_frames);
    if(!ref)return 2;
    struct sigaction action={0};
    action.sa_handler=on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT,&action,NULL);
    sigaction(SIGTERM,&action,NULL);
    signal(SIGPIPE,SIG_IGN);
    audiocpp_registry *registry=NULL;
    audiocpp_model *model=NULL;
    audiocpp_session *session=NULL;
    audiocpp_request *request=NULL;
    audiocpp_model_config config={"meanvc2",NULL,NULL,NULL};
    audiocpp_backend_config backend={"cpu",0,4};
    check(audiocpp_registry_create(NULL,&registry),"registry");
    check(audiocpp_model_load(registry,argv[1],&config,NULL,&model),"load");
    if(!audiocpp_model_supports(model,"vc","streaming")) {
        fprintf(stderr,"MVC2_ERROR streaming unsupported\n");return 3;
    }
    check(audiocpp_session_create(model,"vc","streaming",&backend,NULL,&session),"session");
    request=audiocpp_request_create();
    check(audiocpp_request_set_voice_audio(request,ref,ref_frames,16000,1),"voice");
    fprintf(stderr,"MVC2_PHASE warming session (heavy allocation)\n");
    check(audiocpp_stream_start(session,request),"warm");
    free(ref);ref=NULL;
    int listen_fd=-1,client=-1;
    struct sockaddr_un addr={0};
    addr.sun_family=AF_UNIX;
    strncpy(addr.sun_path,socket_path,sizeof(addr.sun_path)-1);
    if(lstat(socket_path,&st)==0 || errno!=ENOENT) {
        fprintf(stderr,"MVC2_ERROR socket now occupied\n");goto cleanup;
    }
    listen_fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
    if(listen_fd<0) {perror("socket");goto cleanup;}
    mode_t old_mask=umask(0077);
    int bound=bind(listen_fd,(struct sockaddr*)&addr,sizeof(addr));
    umask(old_mask);
    if(bound!=0) {perror("bind");goto cleanup;}
    if(listen(listen_fd,1)!=0) {perror("listen");goto cleanup;}
    fprintf(stderr,"MVC2_READY socket=%s pid=%ld\n",socket_path,(long)getpid());
    fflush(stderr);
    client=accept4(listen_fd,NULL,NULL,SOCK_CLOEXEC);
    if(client<0) {perror("accept");goto cleanup;}
    struct ucred cred={0};socklen_t cred_len=sizeof(cred);
    if(getsockopt(client,SOL_SOCKET,SO_PEERCRED,&cred,&cred_len)!=0 || cred.uid!=getuid()) {
        fprintf(stderr,"MVC2_ERROR client uid mismatch\n");goto cleanup;
    }
    struct timeval limit={5,0};
    setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,&limit,sizeof(limit));
    setsockopt(client,SOL_SOCKET,SO_SNDTIMEO,&limit,sizeof(limit));
    fprintf(stderr,"MVC2_CLIENT uid=%ld pid=%ld\n",(long)cred.uid,(long)cred.pid);
    while(!stopped) {
        struct mvc2_header in_h={0},out_h={MVC2_MAGIC,0};
        float input[MVC2_FRAMES]={0};
        if(exact(client,&in_h,sizeof(in_h),0)!=0)break;
        if(in_h.magic!=MVC2_MAGIC || in_h.frames!=MVC2_FRAMES) {
            fprintf(stderr,"MVC2_ERROR invalid client frame\n");break;
        }
        if(exact(client,input,sizeof(input),0)!=0)break;
        audiocpp_event *event=NULL;
        if(audiocpp_stream_push(session,input,MVC2_FRAMES,16000,1,0,&event)!=AUDIOCPP_OK) {
            fprintf(stderr,"MVC2_ERROR inference: %s\n",audiocpp_last_error());break;
        }
        const float *samples=NULL;size_t frames=0;int rate=0,channels=0;
        if(event) {
            audiocpp_status status=audiocpp_result_audio(audiocpp_event_as_result(event),
                                           &samples,&frames,&rate,&channels);
            if(status==AUDIOCPP_ERR_NOT_AVAILABLE)frames=0;
            else if(status!=AUDIOCPP_OK || rate!=16000 || channels!=1 || frames>MVC2_MAX_OUTPUT) {
                fprintf(stderr,"MVC2_ERROR invalid inference output\n");
                audiocpp_event_free(event);break;
            }
        }
        out_h.frames=(uint32_t)frames;
        int good=(exact(client,&out_h,sizeof(out_h),1)==0);
        if(good && frames) good=(exact(client,(void*)samples,frames*4,1)==0);
        audiocpp_event_free(event);
        if(!good)break;
    }
cleanup:
    if(client>=0)close(client);
    if(listen_fd>=0)close(listen_fd);
    struct stat s;
    if(lstat(socket_path,&s)==0 && S_ISSOCK(s.st_mode) && s.st_uid==getuid())
        unlink(socket_path);
    audiocpp_request_free(request);
    audiocpp_session_free(session);
    audiocpp_model_free(model);
    audiocpp_registry_free(registry);
    fprintf(stderr,"MVC2_STOPPED\n");
    return 0;
}

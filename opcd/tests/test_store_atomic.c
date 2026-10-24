#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../store.h"
#include "../store_async.h"

int __real_fsync(int fd);
int __real_rename(const char *oldpath, const char *newpath);
int __real_close(int fd);
static int fail_file_sync, fail_dir_sync, fail_rename, fail_file_close;
static int failed_close_fd=-1, repeated_close;
static int file_syncs, dir_syncs, renames, failures;
#define CHECK(c, label) do { if (c) printf("PASS %s\n", label); \
    else { fprintf(stderr,"FAIL %s\n",label); failures++; } } while (0)

int __wrap_fsync(int fd)
{
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        dir_syncs++;
        if (fail_dir_sync) { errno = EIO; return -1; }
    } else {
        file_syncs++;
        if (fail_file_sync) { errno = ENOSPC; return -1; }
    }
    return __real_fsync(fd);
}
int __wrap_rename(const char *oldpath, const char *newpath)
{
    renames++;
    if (fail_rename) { errno = EIO; return -1; }
    return __real_rename(oldpath,newpath);
}
int __wrap_close(int fd)
{
    if (fd == failed_close_fd) repeated_close++;
    struct stat st;
    int is_file = fstat(fd,&st)==0 && S_ISREG(st.st_mode);
    int rc=__real_close(fd);
    if (fail_file_close && is_file) {
        fail_file_close=0;
        failed_close_fd=fd;
        errno=EIO;
        return -1;
    }
    return rc;
}
static int async_result(opc_store_async_t *writer, const char *path,
                        uint64_t token, opc_store_async_done_t *out)
{
    if (opc_store_async_submit(writer,path,"async",5,0600,token)!=0) return -1;
    struct pollfd fd={opc_store_async_event_fd(writer),POLLIN,0};
    if (poll(&fd,1,3000)!=1) return -1;
    return opc_store_async_drain(writer,out,1)==1 ? 0 : -1;
}
static int equals(const char *path, const char *text)
{
    char buf[64]; ssize_t n=opc_store_read_all(path,buf,sizeof buf);
    return n==(ssize_t)strlen(text) && memcmp(buf,text,(size_t)n)==0;
}
static void reset(void)
{
    fail_file_sync=fail_dir_sync=fail_rename=fail_file_close=0;
    failed_close_fd=-1; repeated_close=0;
    file_syncs=dir_syncs=renames=0;
}
int main(void)
{
    char dir[]="/tmp/opcd-store-atomic-XXXXXX";
    if (!mkdtemp(dir)) return 2;
    char path[256],temp[300];
    snprintf(path,sizeof path,"%s/config",dir);
    snprintf(temp,sizeof temp,"%s.tmp.%d",path,(int)getpid());
    reset();
    CHECK(opc_store_write_atomic(path,"old",3,0600)==0 && equals(path,"old"),
          "successful atomic write installs exact bytes");
    CHECK(file_syncs==1 && renames==1 && dir_syncs==1,
          "success synchronizes file then installed directory entry");
    struct stat st;
    CHECK(stat(path,&st)==0 && (st.st_mode&0777)==0600,
          "installed config retains private mode");
    reset();fail_file_sync=1;
    CHECK(opc_store_write_atomic(path,"new",3,0600)==-1 && errno==ENOSPC,
          "file sync failure surfaces original errno");
    CHECK(equals(path,"old") && renames==0 && access(temp,F_OK)!=0,
          "file sync failure preserves old bytes and removes temporary file");
    reset();fail_rename=1;
    CHECK(opc_store_write_atomic(path,"new",3,0600)==-1 && errno==EIO,
          "rename failure surfaces original errno");
    CHECK(equals(path,"old") && dir_syncs==0 && access(temp,F_OK)!=0,
          "rename failure preserves old bytes and cleans temporary file");
    reset();fail_dir_sync=1;
    CHECK(opc_store_write_atomic(path,"new",3,0600)==-1 && errno==EIO,
          "directory sync failure cannot be reported as durable success");
    CHECK(equals(path,"new") && renames==1 && access(temp,F_OK)!=0,
          "directory sync failure leaves newly installed bytes visible");
    opc_store_outcome_t outcome;
    reset();fail_file_sync=1;
    CHECK(opc_store_write_atomic_ex(path,"file-failed",11,0600,&outcome)==-1 &&
          outcome==OPC_STORE_UNCHANGED && equals(path,"new"),
          "file sync error reports unchanged target outcome");
    reset();fail_rename=1;
    CHECK(opc_store_write_atomic_ex(path,"rename-failed",13,0600,&outcome)==-1 &&
          outcome==OPC_STORE_UNCHANGED && equals(path,"new"),
          "rename error reports unchanged target outcome");
    reset();fail_file_close=1;
    CHECK(opc_store_write_atomic_ex(path,"close-failed",12,0600,&outcome)==-1 &&
          errno==EIO && outcome==OPC_STORE_UNCHANGED && repeated_close==0,
          "failed Linux file close is not retried and target remains uninstalled");
    failed_close_fd=-1;
    CHECK(equals(path,"new") && access(temp,F_OK)!=0,
          "close error preserves prior bytes and cleans candidate");
    reset();fail_dir_sync=1;
    CHECK(opc_store_write_atomic_ex(path,"installed",9,0600,&outcome)==-1 &&
          outcome==OPC_STORE_REPLACED_UNSYNCED && equals(path,"installed"),
          "post-rename error reports visible new target and unconfirmed durability");
    reset();
    CHECK(opc_store_write_atomic_ex(path,"durable",7,0600,&outcome)==0 &&
          outcome==OPC_STORE_DURABLE && equals(path,"durable"),
          "both syncs report durable installed target");
    CHECK(opc_store_write_atomic_ex(NULL,"x",1,0600,&outcome)==-1 &&
          errno==EINVAL && outcome==OPC_STORE_UNCHANGED,
          "invalid write cannot leave an old success outcome");
    {
        /* #194: the candidate name may be planted by the directory's owner
         * (the FTP account's home); a link there must not be followed. */
        char victim[300], lpath[300], ltemp[320];
        snprintf(victim,sizeof victim,"%s/victim",dir);
        snprintf(lpath,sizeof lpath,"%s/linked",dir);
        snprintf(ltemp,sizeof ltemp,"%s.tmp.%d",lpath,(int)getpid());
        reset();
        CHECK(opc_store_write_atomic(victim,"keep",4,0600)==0 && symlink(victim,ltemp)==0,
              "a link to another file is planted at the candidate name");
        CHECK(opc_store_write_atomic(lpath,"linked",6,0600)==0 && equals(lpath,"linked") &&
              equals(victim,"keep") && access(ltemp,F_OK)!=0,
              "the write replaces the link instead of writing through it");
        unlink(victim);
        unlink(lpath);
    }

    opc_store_async_t *writer=opc_store_async_create();
    CHECK(writer!=NULL,"async outcome test creates real worker");
    if (writer) {
        opc_store_async_done_t done;
        reset();fail_file_sync=1;
        CHECK(async_result(writer,path,501,&done)==0 && done.token==501 &&
              done.result==-1 && done.saved_errno==ENOSPC &&
              done.outcome==OPC_STORE_UNCHANGED && equals(path,"durable"),
              "async completion propagates pre-rename error without replacing bytes");
        reset();fail_dir_sync=1;
        CHECK(async_result(writer,path,502,&done)==0 && done.token==502 &&
              done.result==-1 && done.saved_errno==EIO &&
              done.outcome==OPC_STORE_REPLACED_UNSYNCED && equals(path,"async"),
              "async completion propagates visible post-rename failure outcome");
        reset();
        CHECK(async_result(writer,path,503,&done)==0 && done.token==503 &&
              done.result==0 && done.saved_errno==0 && done.outcome==OPC_STORE_DURABLE,
              "async completion success includes confirmed durability");
        opc_store_async_destroy(writer);
    }
    reset();
    char cwd[4096];
    CHECK(getcwd(cwd,sizeof cwd)!=NULL && chdir(dir)==0,
          "relative path test enters isolated directory");
    CHECK(opc_store_write_atomic("relative","rel",3,0600)==0 &&
          equals("relative","rel") && dir_syncs==1,
          "relative target synchronizes its current directory");
    unlink("relative");
    CHECK(chdir(cwd)==0,"relative path test restores cwd");
    unlink(path);rmdir(dir);
    return failures ? 1 : 0;
}

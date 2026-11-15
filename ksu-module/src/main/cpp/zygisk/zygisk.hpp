// Zygisk module API — compatible reimplementation of the ISC-licensed
// topjohnwu/zygisk-module-sample header (ZYGISK_API_VERSION 4 ABI).
#pragma once

#include <jni.h>
#include <sys/types.h>
#include <cstdint>

#define ZYGISK_API_VERSION 4

namespace zygisk {

struct Api;
struct AppSpecializeArgs;
struct ServerSpecializeArgs;

class ModuleBase {
public:
    virtual void onLoad(Api *, JNIEnv *) {}
    virtual void preAppSpecialize(AppSpecializeArgs *) {}
    virtual void postAppSpecialize(const AppSpecializeArgs *) {}
    virtual void preServerSpecialize(ServerSpecializeArgs *) {}
    virtual void postServerSpecialize(const ServerSpecializeArgs *) {}
};

struct AppSpecializeArgs {
    jint &uid;
    jint &gid;
    jintArray &gids;
    jint &runtime_flags;
    jobjectArray &rlimits;
    jint &mount_external;
    jstring &se_info;
    jstring &nice_name;
    jstring &instruction_set;
    jstring &app_data_dir;

    jintArray *const fds_to_ignore = nullptr;
    jboolean *const is_child_zygote = nullptr;
    jboolean *const is_top_app = nullptr;
    jobjectArray *const pkg_data_info_list = nullptr;
    jobjectArray *const whitelisted_data_info_list = nullptr;
    jboolean *const mount_data_dirs = nullptr;
    jboolean *const mount_storage_dirs = nullptr;

    AppSpecializeArgs() = delete;
};

struct ServerSpecializeArgs {
    jint &uid;
    jint &gid;
    jintArray &gids;
    jint &runtime_flags;
    jlong &permitted_capabilities;
    jlong &effective_capabilities;

    ServerSpecializeArgs() = delete;
};

enum Option : int {
    FORCE_DENYLIST_UNMOUNT = 0,
    DLCLOSE_MODULE_LIBRARY = 1,
};

enum StateFlag : uint32_t {
    PROCESS_GRANTED_ROOT = (1u << 0),
    PROCESS_ON_DENYLIST  = (1u << 1),
};

namespace internal {

struct module_abi {
    long api_version;
    ModuleBase *impl;

    void (*preAppSpecialize)(ModuleBase *, AppSpecializeArgs *);
    void (*postAppSpecialize)(ModuleBase *, const AppSpecializeArgs *);
    void (*preServerSpecialize)(ModuleBase *, ServerSpecializeArgs *);
    void (*postServerSpecialize)(ModuleBase *, const ServerSpecializeArgs *);

    module_abi(ModuleBase *module) : api_version(ZYGISK_API_VERSION), impl(module) {
        preAppSpecialize     = [](ModuleBase *m, AppSpecializeArgs *a) { m->preAppSpecialize(a); };
        postAppSpecialize    = [](ModuleBase *m, const AppSpecializeArgs *a) { m->postAppSpecialize(a); };
        preServerSpecialize  = [](ModuleBase *m, ServerSpecializeArgs *a) { m->preServerSpecialize(a); };
        postServerSpecialize = [](ModuleBase *m, const ServerSpecializeArgs *a) { m->postServerSpecialize(a); };
    }
};

struct api_table {
    void *impl;
    bool (*registerModule)(api_table *, module_abi *);

    void (*hookJniNativeMethods)(JNIEnv *, const char *, JNINativeMethod *, int);
    void (*pltHookRegister)(dev_t, ino_t, const char *, void *, void **);
    bool (*exemptFd)(int);
    bool (*pltHookCommit)();
    int  (*connectCompanion)(void *);
    void (*setOption)(void *, Option);
    int  (*getModuleDir)(void *);
    uint32_t (*getFlags)(void *);
};

template <class T>
void entry_impl(api_table *table, JNIEnv *env);

}  // namespace internal

struct Api {
    int  connectCompanion() { return tbl->connectCompanion ? tbl->connectCompanion(tbl->impl) : -1; }
    int  getModuleDir()     { return tbl->getModuleDir ? tbl->getModuleDir(tbl->impl) : -1; }
    void setOption(Option opt) { if (tbl->setOption) tbl->setOption(tbl->impl, opt); }
    uint32_t getFlags()     { return tbl->getFlags ? tbl->getFlags(tbl->impl) : 0; }
    bool exemptFd(int fd)   { return tbl->exemptFd && tbl->exemptFd(fd); }
    void hookJniNativeMethods(JNIEnv *env, const char *className, JNINativeMethod *methods, int n) {
        if (tbl->hookJniNativeMethods) tbl->hookJniNativeMethods(env, className, methods, n);
    }
    void pltHookRegister(dev_t dev, ino_t inode, const char *symbol, void *newFunc, void **oldFunc) {
        if (tbl->pltHookRegister) tbl->pltHookRegister(dev, inode, symbol, newFunc, oldFunc);
    }
    bool pltHookCommit() { return tbl->pltHookCommit && tbl->pltHookCommit(); }

private:
    internal::api_table *tbl;
    template <class T> friend void internal::entry_impl(internal::api_table *, JNIEnv *);
};

namespace internal {

template <class T>
void entry_impl(api_table *table, JNIEnv *env) {
    static Api api;
    api.tbl = table;
    static T module;
    ModuleBase *base = &module;
    static module_abi abi(base);
    if (!table->registerModule(table, &abi)) return;
    base->onLoad(&api, env);
}

}  // namespace internal

}  // namespace zygisk

#define REGISTER_ZYGISK_MODULE(clazz) \
extern "C" [[gnu::visibility("default")]] [[gnu::used]] \
void zygisk_module_entry(zygisk::internal::api_table *table, JNIEnv *env) { \
    zygisk::internal::entry_impl<clazz>(table, env); \
}

#define REGISTER_ZYGISK_COMPANION(func) \
extern "C" [[gnu::visibility("default")]] [[gnu::used]] \
void zygisk_companion_entry(int client) { func(client); }

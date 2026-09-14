#include <jni.h>
#include <android/log.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include "zygisk.hpp"

#define LOG_TAG "CODM-TutorialSkip"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static constexpr const char *TARGET_PROCESS = "com.garena.game.codm";
static constexpr const char *TARGET_LIBRARY = "libunity.so";

static constexpr uintptr_t RVA_GET_CURRENT_TUTORIAL   = 0x9DE143C;
static constexpr uintptr_t RVA_RESET_CURRENT_TUTORIAL = 0x9DE1494;
static constexpr uintptr_t RVA_IS_FINISHED_INT        = 0x9DE3174;
static constexpr uintptr_t RVA_IS_FINISHED_TYPE       = 0x9DE31D4;

static constexpr uint32_t ARM64_MOV_W0_0 = 0x52800000;
static constexpr uint32_t ARM64_MOV_W0_1 = 0x52800020;
static constexpr uint32_t ARM64_RET      = 0xD65F03C0;

static constexpr bool PATCH_GET_CURRENT = true;
static constexpr bool PATCH_IS_FINISHED = true;
static constexpr bool PATCH_RESET       = true;

static uintptr_t find_library_base(const char *library) {
    FILE *fp = fopen("/proc/self/maps", "r");
    if (!fp) return 0;

    char line[1024];
    uintptr_t best = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (!strstr(line, library)) continue;

        uintptr_t start = 0, end = 0;
        unsigned long offset = 0;
        char perms[8] = {};

        if (sscanf(line, "%lx-%lx %7s %lx", &start, &end, perms, &offset) >= 4) {
            if (offset == 0) {
                best = start;
                break;
            }
            if (!best || start < best) best = start;
        }
    }

    fclose(fp);
    return best;
}

static bool patch32(uintptr_t address, uint32_t value) {
    long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) return false;

    uintptr_t page = address & ~(static_cast<uintptr_t>(pageSize) - 1);

    if (mprotect(reinterpret_cast<void *>(page),
                 static_cast<size_t>(pageSize),
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        LOGE("mprotect failed @%p: %s",
             reinterpret_cast<void *>(address), strerror(errno));
        return false;
    }

    *reinterpret_cast<volatile uint32_t *>(address) = value;

    __builtin___clear_cache(
        reinterpret_cast<char *>(address),
        reinterpret_cast<char *>(address + 4)
    );

    mprotect(reinterpret_cast<void *>(page),
             static_cast<size_t>(pageSize),
             PROT_READ | PROT_EXEC);

    return true;
}

static bool patch_return_bool(uintptr_t base, uintptr_t rva, bool value) {
    uintptr_t addr = base + rva;

    if (!patch32(addr, value ? ARM64_MOV_W0_1 : ARM64_MOV_W0_0))
        return false;

    return patch32(addr + 4, ARM64_RET);
}

static bool patch_void_return(uintptr_t base, uintptr_t rva) {
    return patch32(base + rva, ARM64_RET);
}

static void apply_patches(uintptr_t base) {
    int ok = 0, fail = 0;

    if (PATCH_IS_FINISHED) {
        patch_return_bool(base, RVA_IS_FINISHED_INT, true) ? ++ok : ++fail;
        patch_return_bool(base, RVA_IS_FINISHED_TYPE, true) ? ++ok : ++fail;
    }

    if (PATCH_GET_CURRENT)
        patch_return_bool(base, RVA_GET_CURRENT_TUTORIAL, false) ? ++ok : ++fail;

    if (PATCH_RESET)
        patch_void_return(base, RVA_RESET_CURRENT_TUTORIAL) ? ++ok : ++fail;

    LOGI("patch complete: ok=%d fail=%d", ok, fail);
}

static void *worker(void *) {
    LOGI("worker started; waiting for %s", TARGET_LIBRARY);

    for (int i = 0; i < 300; ++i) {
        uintptr_t base = find_library_base(TARGET_LIBRARY);

        if (base) {
            LOGI("%s base=%p", TARGET_LIBRARY,
                 reinterpret_cast<void *>(base));

            usleep(500000);
            apply_patches(base);
            return nullptr;
        }

        usleep(200000);
    }

    LOGE("%s not found", TARGET_LIBRARY);
    return nullptr;
}

class CodmTutorialSkip : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        api_ = api;
        env_ = env;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        target_ = false;

        if (!env_ || !args || !args->nice_name)
            return;

        const char *name = env_->GetStringUTFChars(args->nice_name, nullptr);

        if (name) {
            target_ = strcmp(name, TARGET_PROCESS) == 0;

            if (target_)
                LOGI("target process detected: %s", name);

            env_->ReleaseStringUTFChars(args->nice_name, name);
        }

        if (!target_)
            api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        if (!target_) return;

        pthread_t t{};
        int rc = pthread_create(&t, nullptr, worker, nullptr);

        if (rc != 0) {
            LOGE("pthread_create failed: %d", rc);
            return;
        }

        pthread_detach(t);
    }

private:
    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
    bool target_ = false;
};

REGISTER_ZYGISK_MODULE(CodmTutorialSkip)

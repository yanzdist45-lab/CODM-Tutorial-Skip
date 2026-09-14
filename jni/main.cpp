#include <jni.h>
#include <android/log.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <ctime>

#include "zygisk.hpp"

#define LOG_TAG "CODM-TutorialSkip"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static constexpr const char *TARGET_PROCESS = "com.garena.game.codm";
static constexpr const char *TARGET_LIBRARY = "libunity.so";

static constexpr const char *STATUS_FILE =
    "/data/adb/modules/codm_tutorial_skip/status.log";

static constexpr uintptr_t RVA_GET_CURRENT_TUTORIAL   = 0x9DE143C;
static constexpr uintptr_t RVA_RESET_CURRENT_TUTORIAL = 0x9DE1494;
static constexpr uintptr_t RVA_IS_FINISHED_INT        = 0x9DE3174;
static constexpr uintptr_t RVA_IS_FINISHED_TYPE       = 0x9DE31D4;

static constexpr uint32_t ARM64_MOV_W0_0 = 0x52800000;
static constexpr uint32_t ARM64_MOV_W0_1 = 0x52800020;
static constexpr uint32_t ARM64_RET       = 0xD65F03C0;

static constexpr bool PATCH_GET_CURRENT = true;
static constexpr bool PATCH_IS_FINISHED = true;
static constexpr bool PATCH_RESET       = true;

static void write_status(const char *msg) {
    FILE *fp = fopen(STATUS_FILE, "a");
    if (!fp) return;

    time_t now = time(nullptr);
    struct tm tm_buf{};
    localtime_r(&now, &tm_buf);

    char timebuf[64];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm_buf);

    fprintf(fp, "[%s] %s\n", timebuf, msg);
    fclose(fp);
}

static uintptr_t find_library_base(const char *library) {
    FILE *fp = fopen("/proc/self/maps", "r");
    if (!fp) return 0;

    char line[1024];
    uintptr_t best = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (!strstr(line, library))
            continue;

        uintptr_t start = 0;
        uintptr_t end = 0;
        unsigned long offset = 0;
        char perms[8] = {};

        if (sscanf(line, "%lx-%lx %7s %lx",
                   &start, &end, perms, &offset) >= 4) {

            if (offset == 0) {
                best = start;
                break;
            }

            if (!best || start < best)
                best = start;
        }
    }

    fclose(fp);
    return best;
}

static bool patch32(uintptr_t address, uint32_t value) {
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
        return false;

    uintptr_t page =
        address & ~(static_cast<uintptr_t>(page_size) - 1);

    if (mprotect(
            reinterpret_cast<void *>(page),
            static_cast<size_t>(page_size),
            PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {

        return false;
    }

    *reinterpret_cast<volatile uint32_t *>(address) = value;

    __builtin___clear_cache(
        reinterpret_cast<char *>(address),
        reinterpret_cast<char *>(address + 4)
    );

    mprotect(
        reinterpret_cast<void *>(page),
        static_cast<size_t>(page_size),
        PROT_READ | PROT_EXEC
    );

    return true;
}

static bool patch_return_bool(
    uintptr_t base,
    uintptr_t rva,
    bool value
) {
    uintptr_t addr = base + rva;

    if (!patch32(
            addr,
            value ? ARM64_MOV_W0_1 : ARM64_MOV_W0_0))
        return false;

    return patch32(addr + 4, ARM64_RET);
}

static bool patch_void_return(
    uintptr_t base,
    uintptr_t rva
) {
    return patch32(base + rva, ARM64_RET);
}

static bool apply_patches(uintptr_t base) {
    int ok = 0;
    int fail = 0;

    if (PATCH_IS_FINISHED) {
        patch_return_bool(
            base,
            RVA_IS_FINISHED_INT,
            true
        ) ? ++ok : ++fail;

        patch_return_bool(
            base,
            RVA_IS_FINISHED_TYPE,
            true
        ) ? ++ok : ++fail;
    }

    if (PATCH_GET_CURRENT) {
        patch_return_bool(
            base,
            RVA_GET_CURRENT_TUTORIAL,
            false
        ) ? ++ok : ++fail;
    }

    if (PATCH_RESET) {
        patch_void_return(
            base,
            RVA_RESET_CURRENT_TUTORIAL
        ) ? ++ok : ++fail;
    }

    char buf[128];
    snprintf(
        buf,
        sizeof(buf),
        "patch attempt: ok=%d fail=%d base=0x%lx",
        ok,
        fail,
        static_cast<unsigned long>(base)
    );

    write_status(buf);

    return fail == 0;
}

static void *worker(void *) {
    write_status("worker started");

    /*
     * Tunggu libunity sampai 60 detik.
     */
    uintptr_t base = 0;

    for (int i = 0; i < 300; ++i) {
        base = find_library_base(TARGET_LIBRARY);

        if (base)
            break;

        usleep(200000);
    }

    if (!base) {
        write_status("ERROR: libunity.so not found");
        return nullptr;
    }

    char buf[128];
    snprintf(
        buf,
        sizeof(buf),
        "libunity.so found: base=0x%lx",
        static_cast<unsigned long>(base)
    );
    write_status(buf);

    /*
     * Kasih Unity waktu sedikit buat selesai init.
     */
    sleep(1);

    /*
     * Retry patch tiap 1 detik selama 20 detik.
     *
     * Ini sengaja supaya kalau CODM menimpa / re-init
     * state tutorial saat login, patch dipasang lagi.
     */
    for (int i = 1; i <= 20; ++i) {
        uintptr_t current_base =
            find_library_base(TARGET_LIBRARY);

        if (!current_base) {
            write_status("library disappeared");
            sleep(1);
            continue;
        }

        char attempt[64];
        snprintf(
            attempt,
            sizeof(attempt),
            "retry %d/20",
            i
        );
        write_status(attempt);

        apply_patches(current_base);

        sleep(1);
    }

    write_status("retry sequence finished");

    return nullptr;
}

class CodmTutorialSkip : public zygisk::ModuleBase {
public:
    void onLoad(
        zygisk::Api *api,
        JNIEnv *env
    ) override {
        api_ = api;
        env_ = env;
    }

    void preAppSpecialize(
        zygisk::AppSpecializeArgs *args
    ) override {
        target_ = false;

        if (!env_ || !args || !args->nice_name)
            return;

        const char *name =
            env_->GetStringUTFChars(
                args->nice_name,
                nullptr
            );

        if (name) {
            target_ =
                strcmp(name, TARGET_PROCESS) == 0;

            env_->ReleaseStringUTFChars(
                args->nice_name,
                name
            );
        }

        if (!target_) {
            api_->setOption(
                zygisk::Option::DLCLOSE_MODULE_LIBRARY
            );
        }
    }

    void postAppSpecialize(
        const zygisk::AppSpecializeArgs *
    ) override {
        if (!target_)
            return;

        /*
         * Kosongkan status lama tiap CODM start.
         */
        remove(STATUS_FILE);
        write_status("CODM target detected");

        pthread_t t{};
        int rc =
            pthread_create(
                &t,
                nullptr,
                worker,
                nullptr
            );

        if (rc != 0) {
            write_status("ERROR: pthread_create failed");
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

#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""CPU-only regression checks for Xclipse resource initialization and synchronization.

Run with a C11 compiler and pthreads, without Meson, an NDK, DRM or GPU access:
    python3 bin/xclipse-resource-regressions.py

The actual function bodies are extracted from the worktree and compiled with mocked
allocation/DRM calls. Scheduling hooks expose partial initialization and overlapping
winsys teardown deterministically. This does not build or exercise the full driver.
Use --ref COMMIT --expect-bugs to reproduce the reviewed pre-fix behavior.
"""
from pathlib import Path
import argparse
import json
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--ref', help='Read reviewed C sources from this Git ref instead of the worktree')
parser.add_argument('--expect-bugs', action='store_true', help='Reproduce the pre-fix behavior as a control')
parser.add_argument('--sanitize', action='store_true', help='Enable address and undefined-behavior sanitizers')
args = parser.parse_args()
FIXED = not args.expect_bugs
_temporary = tempfile.TemporaryDirectory(prefix='xclipse-regressions-')
OUT = Path(_temporary.name)

def read_source(path):
    if args.ref:
        return subprocess.check_output(['git', '-C', str(ROOT), 'show', args.ref + ':' + path], text=True)
    return (ROOT / path).read_text()

def function(path, name):
    text = read_source(path)
    m = re.search(r'(?m)^(?:static )?(?:inline )?(?:VkResult|bool|int|void)\s*\n?' + name + r'\(', text)
    assert m, name
    start = text.index('{', m.end())
    depth = 1
    end = start + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[m.start():end]

COMMON = '#define EXPECT_FIXED ' + str(int(FIXED)) + '\n' + '''
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <setjmp.h>
#include <sys/mman.h>
typedef int VkResult;
#define VK_SUCCESS 0
#define VK_ERROR_OUT_OF_HOST_MEMORY (-1)
#define VK_ERROR_DEVICE_LOST (-4)
#define VK_ERROR_INITIALIZATION_FAILED (-3)
'''

def run(name, body):
    source = OUT / (name + '.c')
    binary = OUT / name
    source.write_text(COMMON + body)
    sanitizers = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if args.sanitize else []
    subprocess.run(['cc', '-std=c11', '-O0', '-g', '-pthread', '-Wall', '-Wextra',
                    '-Wno-unused-parameter', '-Wno-unused-function', '-Werror',
                    *sanitizers, str(source), '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], text=True, capture_output=True, timeout=5)
    if result.returncode:
        raise RuntimeError(name + ':\n' + result.stdout + result.stderr)
    print(result.stdout.strip(), flush=True)
    return result.stdout.strip()

meta = 'src/amd/vulkan/meta/radv_meta_bc_decode.c'
bc = '''
#define BC_EAC_PAIRS 65536u
#define BC_EAC_LUT_BYTES (BC_EAC_PAIRS * 16u)
#define BC_HIST_BYTES (4096u + 1024u * 64u * 4u)
#define RADEON_DOMAIN_GTT 1
#define RADEON_FLAG_CPU_ACCESS 1
#define RADEON_FLAG_NO_INTERPROCESS_SHARING 2
#define RADV_BO_PRIORITY_UPLOAD_BUFFER 0
static uint32_t eac_lut[BC_EAC_PAIRS], eac_remap[BC_EAC_PAIRS * 2];
struct radeon_winsys_bo { void *map; uint64_t va; };
struct radv_device {
    void *ws;
    struct { pthread_mutex_t mtx; struct {
        struct radeon_winsys_bo *hist_bo;
        uint32_t *hist_ptr;
        uint64_t hist_va;
        struct radeon_winsys_bo *eac_lut_bo;
        uint64_t eac_lut_va;
    } xclipse_bc; } meta_state;
};
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static bool allocated, release_create, second_lock, second_finished;
static uint64_t pause_size, second_va;
static bool second_ready;
static _Thread_local int role;
static int mode;
static int mtx_lock(pthread_mutex_t *m) {
    if (role == 2) {
        pthread_mutex_lock(&gate);
        second_lock = true;
        pthread_cond_broadcast(&cond);
        pthread_mutex_unlock(&gate);
    }
    return pthread_mutex_lock(m);
}
#define mtx_unlock pthread_mutex_unlock
static int radv_bo_create(struct radv_device *d, void *p, uint64_t size,
                         unsigned a, int dom, int flags, int pri, int zero, bool track,
                         struct radeon_winsys_bo **out) {
    *out = calloc(1, sizeof(**out));
    (*out)->map = calloc(1, size);
    (*out)->va = 0x100000;
    if (size == pause_size && role == 1) {
        pthread_mutex_lock(&gate);
        allocated = true;
        pthread_cond_broadcast(&cond);
        while (!release_create) pthread_cond_wait(&cond, &gate);
        pthread_mutex_unlock(&gate);
    }
    return VK_SUCCESS;
}
static void *radv_buffer_map(void *w, struct radeon_winsys_bo *b) { return b->map; }
static uint64_t radv_buffer_get_va(struct radeon_winsys_bo *b) { return b->va; }
static void radv_bo_destroy(struct radv_device *d, void *a, struct radeon_winsys_bo *b) {
    free(b->map); free(b);
}
'''
bc += function(meta, 'bc_hist_init') + '\n' + function(meta, 'bc_eac_lut_init')
if 'bc_resources_init(' in read_source(meta):
    bc += '\n' + function(meta, 'bc_resources_init')
else:
    # The old caller invoked these directly, without a shared-resource lock.
    bc += '\nstatic bool bc_resources_init(struct radv_device *d) { return bc_hist_init(d) && bc_eac_lut_init(d); }\n'
bc += '''
struct worker_args { struct radv_device *d; int role; };
static void *worker(void *ptr) {
    struct worker_args *a = ptr;
    struct radv_device *d = a->d;
    role = a->role;
    bool ready = bc_resources_init(d);
    if (role == 2) {
        pthread_mutex_lock(&gate);
        second_ready = ready;
        second_va = mode ? d->meta_state.xclipse_bc.eac_lut_va : d->meta_state.xclipse_bc.hist_va;
        second_finished = true;
        pthread_cond_broadcast(&cond);
        pthread_mutex_unlock(&gate);
    } else {
        assert(ready);
    }
    return NULL;
}
int main(void) {
    for (mode = 0; mode < 2; mode++) {
        struct radv_device d = {0};
        assert(!pthread_mutex_init(&d.meta_state.mtx, NULL));
        allocated = release_create = second_lock = second_finished = false;
        second_ready = false;
        second_va = 0;
        pause_size = mode ? BC_EAC_LUT_BYTES : BC_HIST_BYTES;
        pthread_t a, b;
        struct worker_args aa = {&d, 1}, bb = {&d, 2};
        assert(!pthread_create(&a, NULL, worker, &aa));
        pthread_mutex_lock(&gate);
        while (!allocated) pthread_cond_wait(&cond, &gate);
        pthread_mutex_unlock(&gate);
        assert(!pthread_create(&b, NULL, worker, &bb));
        pthread_mutex_lock(&gate);
        while (!second_lock && !second_finished) pthread_cond_wait(&cond, &gate);
        if (EXPECT_FIXED) {
            assert(second_lock && !second_finished);
        } else {
            assert(second_finished && second_ready && second_va == 0);
        }
        release_create = true;
        pthread_cond_broadcast(&cond);
        pthread_mutex_unlock(&gate);
        pthread_join(a, NULL);
        pthread_join(b, NULL);
        assert(second_ready);
        if (EXPECT_FIXED) assert(second_va != 0);
        assert(d.meta_state.xclipse_bc.hist_va && d.meta_state.xclipse_bc.eac_lut_va);
        printf("%s BC %s initialization: concurrent recorder %s\\n", EXPECT_FIXED ? "PASS" : "REPRO", mode ? "EAC LUT" : "histogram", EXPECT_FIXED ? "waits for complete resource initialization" : "observes GPU VA=0");
        radv_bo_destroy(&d, NULL, d.meta_state.xclipse_bc.eac_lut_bo);
        radv_bo_destroy(&d, NULL, d.meta_state.xclipse_bc.hist_bo);
        pthread_mutex_destroy(&d.meta_state.mtx);
    }
}
'''
results = [run('bc_initialization_race', bc)]

cs = 'src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c'
sync = '''
enum amd_ip_type { AMD_IP_GFX };
struct radv_amdgpu_winsys { void *dev; struct { bool has_timeline_syncobj; } info; };
struct radv_amdgpu_ctx { struct radv_amdgpu_winsys *ws; bool queue_syncobj_wait[1][1]; };
struct radv_winsys_sem_counts { unsigned syncobj_count, timeline_syncobj_count; uint32_t *syncobj; uint64_t *points; };
struct radv_winsys_sem_info { bool cs_emit_signal, cs_emit_wait; struct radv_winsys_sem_counts wait, signal; };
static unsigned import_calls, signal_calls, closed_count, merge_calls;
static bool fail_merge;
static unsigned radv_amdgpu_ctx_queue_syncobj(struct radv_amdgpu_ctx *c, unsigned ip, int q) { return 10; }
static int ac_drm_cs_syncobj_export_sync_file(void *d, unsigned s, int *fd) { *fd = (int)s + 100; return 0; }
static int ac_drm_cs_syncobj_export_sync_file2(void *d, unsigned s, uint64_t p, unsigned f, int *fd) { *fd = (int)s + 100; return 0; }
static int ac_drm_cs_syncobj_query2(void *d, uint32_t *h, uint64_t *p, unsigned n, unsigned f) { *p = 0; return 0; }
static int ac_drm_cs_syncobj_transfer(void *d, unsigned dst, uint64_t dp, unsigned src, uint64_t sp, unsigned f) { signal_calls++; return 0; }
static int ac_drm_cs_syncobj_import_sync_file(void *d, unsigned h, int fd) { import_calls++; return 0; }
static int sync_accumulate(const char *n, int *old_fd, int new_fd) { merge_calls++; if (fail_merge) { errno = ENOMEM; return -1; } return 0; }
static int mock_close(int fd) { closed_count++; return 0; }
#define close mock_close
'''
sync += function(cs, 'radv_amdgpu_cs_submit_zero')
sync += '''
int main(void) {
    for (unsigned timeline = 0; timeline < 2; timeline++) {
      for (unsigned failing = 0; failing < 2; failing++) {
        struct radv_amdgpu_winsys ws = { .info.has_timeline_syncobj = timeline };
        struct radv_amdgpu_ctx ctx = { .ws = &ws };
        uint32_t waits[] = {20}, signals[] = {30};
        uint64_t points[] = {1};
        struct radv_winsys_sem_info sem = {
            .wait = { .syncobj_count = !timeline, .timeline_syncobj_count = timeline, .syncobj = waits, .points = points },
            .signal = { .syncobj_count = 1, .syncobj = signals }
        };
        import_calls = signal_calls = merge_calls = closed_count = 0;
        fail_merge = failing;
        VkResult r = radv_amdgpu_cs_submit_zero(&ctx, AMD_IP_GFX, 0, &sem);
        assert(merge_calls == 1);
        if (EXPECT_FIXED && failing) {
            assert(r == VK_ERROR_DEVICE_LOST && import_calls == 0 && signal_calls == 0);
            assert(closed_count == 2 && !ctx.queue_syncobj_wait[0][0]);
        } else {
            assert(r == VK_SUCCESS && import_calls > 0 && ctx.queue_syncobj_wait[0][0]);
            assert(timeline ? signal_calls == 1 : import_calls == 2);
        }
        printf("%s %s wait: merge %s\\n", EXPECT_FIXED || !failing ? "PASS" : "REPRO", timeline ? "timeline" : "binary", failing ? (EXPECT_FIXED ? "failure propagated without publishing a signal; both FDs closed" : "failure ignored") : "success preserved");
      }
    }
}
'''
results.append(run('ignored_sync_merge_error', sync))

wpath = 'src/amd/vulkan/winsys/amdgpu/radv_amdgpu_winsys.c'
wf = function(wpath, 'radv_amdgpu_winsys_create')
assigned = re.findall(r'ws->base\.(\w+) = (\w+);', wf)
winsys = '''
typedef uint32_t BITSET_WORD;
typedef int ac_drm_device;
struct radv_amdgpu_alloc_tracker { int x; };
struct radeon_info { struct { bool has_smem_with_null_prt_bug; } compiler_info; };
'''
winsys += 'struct radeon_winsys { ' + ''.join('void (*' + field + ')(void);' for field, name in assigned) + ' };\n'
winsys += '''
struct radv_amdgpu_winsys {
    struct radeon_winsys base; ac_drm_device *dev; int fd;
    struct radv_amdgpu_alloc_tracker *alloc_tracker;
    struct radeon_info info;
    bool chain_ib, debug_all_bos, debug_log_bos, dump_ibs, debug_vm;
    FILE *bo_history_logfile;
    uint32_t vm_timeline_syncobj;
    int vm_ioctl_lock, log_bo_list, log_bo_list_lock;
    uint64_t perftest;
    struct { int lock; } global_bo_list, null_prt_bug, ib_cache;
};
#define BITSET_TEST(a,b) 0
#define mtx_plain 0
#define simple_mtx_init(a,b) ((void)0)
#define u_rwlock_init(a) ((void)0)
#define list_inithead(a) ((void)0)
static ac_drm_device drm_device;
static struct radv_amdgpu_alloc_tracker tracker;
static unsigned deinit_calls, release_calls;
static bool fail_syncobj;
static int ac_drm_device_initialize(int fd, bool v, uint32_t *ma, uint32_t *mi, ac_drm_device **d) { *d = &drm_device; return 0; }
static int ac_drm_device_get_fd(ac_drm_device *d) { return 3; }
static uintptr_t ac_drm_device_get_cookie(ac_drm_device *d) { return 1; }
static struct radv_amdgpu_alloc_tracker *radv_amdgpu_alloc_tracker_acquire(uintptr_t cookie) { return &tracker; }
static void radv_amdgpu_alloc_tracker_release(struct radv_amdgpu_alloc_tracker *t) { release_calls++; }
static void ac_drm_device_deinitialize(ac_drm_device *d) { deinit_calls++; }
static int ac_drm_cs_create_syncobj2(ac_drm_device *d, unsigned flags, uint32_t *h) { if (fail_syncobj) return -ENOMEM; *h = 1; return 0; }
static void radv_amdgpu_bo_init_functions(struct radv_amdgpu_winsys *w) {}
static void radv_amdgpu_cs_init_functions(struct radv_amdgpu_winsys *w) {}
'''
winsys += '\n'.join('static void ' + name + '(void) {}' for field, name in assigned) + '\n' + wf
winsys += '''
int main(void) {
    struct radeon_info info = {0};
    BITSET_WORD debug = 0;
    struct radeon_winsys *out = NULL;
    fail_syncobj = false;
    assert(radv_amdgpu_winsys_create(3, &info, &debug, 0, false, &out) == VK_SUCCESS && out != NULL);
    free(out);
    out = NULL;
    fail_syncobj = true;
    VkResult r = radv_amdgpu_winsys_create(3, &info, &debug, 0, false, &out);
    assert(r == (EXPECT_FIXED ? VK_ERROR_INITIALIZATION_FAILED : VK_SUCCESS));
    assert(out == NULL && deinit_calls == 1 && release_calls == 1);
    puts(EXPECT_FIXED ? "PASS winsys initialization: syncobj failure propagated, resources released, output NULL" : "REPRO winsys initialization: failure reported as VK_SUCCESS with NULL output");
}
'''
results.append(run('winsys_false_success', winsys))

bo_text = read_source('src/amd/vulkan/winsys/amdgpu/radv_amdgpu_bo.c')
start = bo_text.index('#define RADV_XCLIPSE_IBCACHE_BUCKETS')
end = bo_text.index('\nstatic void\nradv_amdgpu_winsys_bo_destroy', start)
cache = '''
typedef pthread_mutex_t simple_mtx_t;
#define SIMPLE_MTX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
#define simple_mtx_lock pthread_mutex_lock
#define simple_mtx_unlock pthread_mutex_unlock
#define RADEON_FLAG_NO_INTERPROCESS_SHARING 1u
#define RADEON_FLAG_XCLIPSE_CACHEABLE 2u
#define RADEON_FLAG_VIRTUAL 4u
#define RADEON_FLAG_REPLAYABLE 8u
#define RADEON_DOMAIN_VRAM 2u
#define RADEON_DOMAIN_GTT 1u
enum radeon_bo_domain { MOCK_DOMAIN = 1 };
struct radeon_winsys { int x; };
struct radeon_winsys_bo { bool is_virtual, imported; uint64_t size; enum radeon_bo_domain initial_domain; };
struct radv_amdgpu_winsys { struct radeon_winsys base; struct { unsigned vram_size_kb; } info; bool xclipse_ibcache_dying; };
struct radv_amdgpu_winsys_bo { struct radeon_winsys_bo base; uint32_t flags; bool emulate_sparse_residency; void *cpu_map; };
'''
cache += bo_text[start:end]
cache += '''
static jmp_buf stuck;
static unsigned attempts, freed;
static void radv_amdgpu_winsys_bo_destroy(struct radeon_winsys *_ws, struct radeon_winsys_bo *_bo) {
    struct radv_amdgpu_winsys *ws = (void *)_ws;
    struct radv_amdgpu_winsys_bo *bo = (void *)_bo;
    if (++attempts > 8) longjmp(stuck, 1);
    if (!radv_xclipse_ibcache_park(ws, bo, 4096)) freed++;
}
int main(void) {
    struct radv_amdgpu_winsys a = {0}, b = {0};
    struct radv_amdgpu_winsys_bo bo = {
        .base = { .size = 4096, .initial_domain = MOCK_DOMAIN },
        .flags = RADEON_FLAG_NO_INTERPROCESS_SHARING | RADEON_FLAG_XCLIPSE_CACHEABLE
    };
    radv_xclipse_ibcache_winsys_begin_destroy(&a);
    /* B begins on another thread while A is between begin/end destroy. */
    radv_xclipse_ibcache_winsys_begin_destroy(&b);
    /* A releases one BO from its per-winsys IB cache. */
    radv_amdgpu_winsys_bo_destroy(&a.base, &bo.base);
    if (!setjmp(stuck)) {
        radv_xclipse_ibcache_winsys_end_destroy(&a);
        assert(EXPECT_FIXED);
        assert(freed == 1 && radv_xclipse_ibcache_count == 0);
        assert(a.xclipse_ibcache_dying && b.xclipse_ibcache_dying);
        radv_xclipse_ibcache_winsys_end_destroy(&b);
        puts("PASS concurrent winsys teardown: A frees its BO while B is also destroying; no re-parking");
        return 0;
    }
    assert(!EXPECT_FIXED && attempts == 9 && freed == 0);
    puts("REPRO concurrent winsys teardown: A's purge repeatedly re-caches its BO while B owns global dying marker");
}
'''
results.append(run('cache_teardown_interleaving', cache))
print(json.dumps({'commit': subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip(),
                  'scope': 'CPU extraction with mocks; no GPU or full-driver build',
                  'results': results}, indent=2))

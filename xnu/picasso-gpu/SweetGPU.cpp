/*
 * SweetGPU: picasso's Adreno 620 for programs to draw with.
 *
 * bsd/dev/picasso_gpu.c brings the GPU up (boot-arg picassogpu=21 or 22) and,
 * with picassogpukeep=1, keeps it on and starts this service. A program that
 * opens it speaks the interface of Linux's msm DRM driver, the one Mesa's
 * freedreno knows: every request is a call of the user client with the
 * request's number (the low byte of the ioctl code) as the selector and the
 * request's structure passed in and back; a buffer's memory is the client
 * memory of the type that is the buffer's handle. This is the ABI of HackiOS's
 * RedwoodGPU (github.com/oosexgang/hackios-xnu-kernel, iokit/Kernel/
 * RedwoodGPU.cpp, APSL 2.0), from which this is ported, so its userland
 * (tools/mesa/redwood-drm, which stands in for libdrm inside Mesa) works
 * against it once it matches "SweetGPU" instead of "RedwoodGPU".
 *
 * What there is: parameters, buffers (made, told their address in the GPU's
 * address space, mapped, given back), submission of command buffers, waiting
 * for one to be done. A buffer is wired kernel memory entered into the 3D
 * core's SMMU context 0; it is write-combined, so what the program writes goes
 * to memory, where the GPU reads it (as Linux maps them for freedreno).
 *
 * One request is not Linux's (HackiOS's 0x7e, surfaceBuffer()): the memory of
 * an IOSurface (SweetSurface.cpp) as a buffer, so the GPU draws into what the
 * display shows, and reads what programs drew with this processor. A
 * surface's memory is cached, the GPU's is not: a submission that names one
 * first pushes it out of this processor's cache (when its seed has changed,
 * and at least once a frame), and the display, before it copies a surface the
 * GPU has drawn into, waits for that and pushes it out again
 * (sweet_gpu_surface_ready(), called by SweetFramebuffer.cpp).
 *
 * Not here yet (HackiOS has them): a cache of given-back buffers, interrupts
 * (waiting polls every 200 us), recovery of a hung core, per-client address
 * spaces (every client's buffers are in the one context, as in HackiOS).
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/system.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <mach/vm_param.h>
#include <sys/proc.h>

#include "SweetSurface.h"

extern "C" {
/* bsd/dev/picasso_gpu.c, "the GPU kept on": called with gGPULock held */
bool sweet_gpu_ready(void);
bool sweet_gpu_parameter(uint32_t which, uint64_t *value);
uint64_t sweet_gpu_address(uint64_t size);
void sweet_gpu_address_free(uint64_t address, uint64_t size);
bool sweet_gpu_map(uint64_t address, uint64_t phys, bool read_only);
void sweet_gpu_mapped(void);
void sweet_gpu_unmap(uint64_t address, uint64_t size);
void sweet_gpu_to_memory(void *start, uint64_t size);
uint32_t sweet_gpu_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count);
bool sweet_gpu_done(uint32_t fence);
uint32_t sweet_gpu_last(void);
void sweet_gpu_report(const char *why);

void SweetGPUStart(void);
bool sweet_gpu_surface_ready(uint32_t id, void *kernel, uint64_t size);
}

/* Linux's include/uapi/drm/msm_drm.h and drm.h: the requests' numbers and structures. */
enum {
	kDrmGemClose            = 0x09,
	kMsmGetParam            = 0x40,
	kMsmSetParam            = 0x41,
	kMsmGemNew              = 0x42,
	kMsmGemInfo             = 0x43,
	kMsmGemCpuPrep          = 0x44,
	kMsmGemCpuFini          = 0x45,
	kMsmGemSubmit           = 0x46,
	kMsmWaitFence           = 0x47,
	kMsmGemMadvise          = 0x48,
	kMsmSubmitQueueNew      = 0x4a,
	kMsmSubmitQueueClose    = 0x4b,
	kMsmSubmitQueueQuery    = 0x4c,
	kSweetGpuSurface        = 0x7e,         /* not Linux's (HackiOS's): an IOSurface's memory as a buffer */
};

struct msm_timespec {
	int64_t seconds, nanoseconds;           /* of the clock that goes on while the machine sleeps */
};
struct msm_param {
	uint32_t pipe, param;
	uint64_t value;
	uint32_t len, pad;
};
struct msm_gem_new {
	uint64_t size;
	uint32_t flags, handle;
};
struct msm_gem_info {
	uint32_t handle, info;
	uint64_t value;
	uint32_t len, pad;
};
struct msm_gem_cpu_prep {
	uint32_t handle, op;
	struct msm_timespec timeout;
};
struct msm_gem_submit_cmd {
	uint32_t type, submit_idx, submit_offset, size, pad, nr_relocs;
	uint64_t relocs;
};
struct msm_gem_submit_bo {
	uint32_t flags, handle;
	uint64_t presumed;
};
struct msm_gem_submit {
	uint32_t flags, fence, nr_bos, nr_cmds;
	uint64_t bos, cmds;
	int32_t fence_fd;
	uint32_t queueid;
	uint64_t in_syncobjs, out_syncobjs;
	uint32_t nr_in_syncobjs, nr_out_syncobjs, syncobj_stride, pad;
};
struct msm_wait_fence {
	uint32_t fence, flags;
	struct msm_timespec timeout;
	uint32_t queueid;
};
struct msm_gem_madvise {
	uint32_t handle, madv, retained;
};
struct msm_submitqueue {
	uint32_t flags, prio, id;
};
struct msm_submitqueue_query {
	uint64_t data;
	uint32_t id, param, len, pad;
};
struct drm_gem_close {
	uint32_t handle, pad;
};
struct sweet_surface_request {                  /* userland/mesa/sweet-drm: struct sweet_drm_surface */
	uint32_t surface;                       /* the IOSurface's id */
	uint32_t handle;                        /* back: the buffer's (SURFACE_DESCRIBE in: only describe it) */
	uint64_t size;                          /* back: how big the buffer is, */
	uint32_t offset, rowBytes;              /* where the pixels start in it and how long a row of them is, */
	uint32_t width, height, pixelFormat, planes;    /* and what they are */
};
#define SURFACE_DESCRIBE        0xffffffffu

#define MSM_INFO_GET_OFFSET     0
#define MSM_INFO_GET_IOVA       1
#define MSM_INFO_SET_NAME       2
#define MSM_INFO_SET_IOVA       4
#define MSM_INFO_GET_FLAGS      5
#define MSM_BO_GPU_READONLY     0x00000002
#define MSM_PREP_NOSYNC         0x04
#define MSM_SUBMIT_CMD_BUF      1
#define MSM_SUBMIT_BO_WRITE     0x0002
#define MSM_SUBMIT_FENCE_FD_OUT 0x20000000

#define GPU_BUFFERS             4096            /* a client's, at most */
#define GPU_BUFFER_MAX          (256u << 20)
#define GPU_SUBMIT_CMDS         128
#define GPU_SUBMIT_BOS          4096
#define GPU_PATIENCE_MS         5000            /* for a wait without a limit */
#define GPU_ASK_AGAIN_US        200             /* while waiting: how long between two looks at the fence */
#define GPU_PUSH_AGAIN_MS       4               /* a surface is pushed out of the cache again this long after the last time */
#define GPU_SURFACE_WRITES      32              /* surfaces whose last GPU write the display may still wait for */

static IOLock *gGPULock;                        /* around everything that is the GPU's (sweet_gpu_...) */
static bool gGPUStopped;                        /* the core stopped answering: nothing is waited for again */
static struct {
	uint32_t id, fence;                     /* a surface the GPU was told to write by submission `fence` */
} gSurfaceWrites[GPU_SURFACE_WRITES];
static uint32_t gSurfaceWriteNext;

/* Wait for submission `fence`; with the lock held, which is let go of while waiting. */
static bool
gpuWait(uint32_t fence, uint64_t milliseconds)
{
	uint64_t began = mach_absolute_time(), patience;

	nanoseconds_to_absolutetime(milliseconds * NSEC_PER_MSEC, &patience);
	for (;;) {
		if (sweet_gpu_done(fence)) {
			return true;
		}
		if (mach_absolute_time() - began >= patience || gGPUStopped) {
			break;
		}
		IOLockUnlock(gGPULock);
		delay_for_interval(GPU_ASK_AGAIN_US, NSEC_PER_USEC);
		IOLockLock(gGPULock);
	}
	/* a wait of a second or more that came to nothing: the core has stopped; said once, and not waited for again */
	if (milliseconds >= 1000 && !gGPUStopped) {
		sweet_gpu_report("no answer");
		gGPUStopped = true;
	}
	return false;
}

/* How long from now a time of msm's is, in milliseconds (at most GPU_PATIENCE_MS); 0: now or past. */
static uint64_t
millisecondsUntil(const struct msm_timespec *when)
{
	uint64_t now;
	uint64_t deadline, left;

	absolutetime_to_nanoseconds(mach_continuous_time(), &now);
	if (when->seconds < 0 || when->nanoseconds < 0 || (uint64_t)when->nanoseconds >= NSEC_PER_SEC) {
		return 0;
	}
	if ((uint64_t)when->seconds > (UINT64_MAX - (uint64_t)when->nanoseconds) / NSEC_PER_SEC)
		return GPU_PATIENCE_MS;
	deadline = (uint64_t)when->seconds * NSEC_PER_SEC + (uint64_t)when->nanoseconds;
	if (deadline <= now) return 0;
	left = (deadline - now) / NSEC_PER_MSEC;
	return left > GPU_PATIENCE_MS ? GPU_PATIENCE_MS : left;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class SweetGPU : public IOService
{
	OSDeclareDefaultStructors(SweetGPU);

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(SweetGPU, IOService);

bool
SweetGPU::start(IOService *provider)
{
	if (!sweet_gpu_ready() || !IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "SweetGPUUserClient");
	setName("SweetGPU");
	setProperty("chip-id", 0x06020001ULL, 32);
	setProperty("GPUModel", "Adreno 620");
	setProperty("hardware-validated", true); /* backend memory-write AND fence self-test passed */
	registerService();
	IOLog("SweetGPU: registered: the GPU is offered to programs\n");
	return true;
}

/* From sweet_gpu.c once the GPU is kept on. */
void
SweetGPUStart(void)
{
	IOService *provider = IOService::getPlatform();
	SweetGPU *gpu;

	gGPULock = IOLockAlloc();
	gpu = OSTypeAlloc(SweetGPU);
	if (gGPULock == NULL || gpu == NULL || provider == NULL || !gpu->init() || !gpu->attach(provider)) {
		OSSafeReleaseNULL(gpu);
		IOLog("SweetGPU: cannot create the service\n");
		return;
	}
	if (!gpu->start(provider)) {
		gpu->detach(provider);
		IOLog("SweetGPU: the service did not start\n");
	}
	gpu->release();
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct gpu_buffer {
	IOMemoryDescriptor *memory;             /* NULL: this handle is free */
	uint64_t address;                       /* the GPU's for it */
	uint64_t size;
	uint32_t flags;
	uint32_t fence;                         /* the last submission that names it (0: none has) */
	SweetSurface *surface;                  /* whose memory it is, when it is an IOSurface's (held) */
	void *kernel;                           /* ... the surface's pixels in the kernel, when they are cached */
	uint32_t seed;                          /* ... its seed when last pushed out of the cache, */
	uint64_t pushed;                        /* ... and when (0: never) */
};

class SweetGPUUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(SweetGPUUserClient);

	struct gpu_buffer *fBuffers;            /* handle n is fBuffers[n - 1] */
	uint32_t fNextBuffer;                   /* where the search for a free handle starts */
	uint32_t fNextQueue;
	uint32_t fSubmits;
	uint32_t fStrangers;                    /* requests that named a buffer the client does not have */

	struct gpu_buffer *buffer(uint32_t handle);
	struct gpu_buffer *newBuffer(IOMemoryDescriptor *memory, uint64_t size, uint32_t flags, uint32_t *handle);
	void freeBuffer(struct gpu_buffer *b);
	IOReturn gemNew(struct msm_gem_new *request);
	IOReturn surfaceBuffer(struct sweet_surface_request *request);
	IOReturn gemInfo(struct msm_gem_info *request);
	IOReturn submit(struct msm_gem_submit *request);

public:
	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	void free(void) APPLE_KEXT_OVERRIDE;
	IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(SweetGPUUserClient, IOUserClient);

bool
SweetGPUUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	return IOUserClient::initWithTask(owningTask, securityID, type);
}

bool
SweetGPUUserClient::start(IOService *provider)
{
	if (gGPULock == NULL || !sweet_gpu_ready() || !IOUserClient::start(provider)) {
		return false;
	}
	fBuffers = IONewZero(struct gpu_buffer, GPU_BUFFERS);
	return fBuffers != NULL;
}

struct gpu_buffer *
SweetGPUUserClient::buffer(uint32_t handle)
{
	if (handle == 0 || handle > GPU_BUFFERS || fBuffers[handle - 1].memory == NULL) {
		if (handle != 0 && fStrangers++ < 16) {
			IOLog("SweetGPU: buffer %u is asked for and is not there\n", handle);
		}
		return NULL;
	}
	return &fBuffers[handle - 1];
}

/*
 * With the lock held. A buffer the core may still be using (it has stopped
 * answering) stays the GPU's for good: its memory may yet be written, and
 * must not be somebody else's by then.
 */
void
SweetGPUUserClient::freeBuffer(struct gpu_buffer *b)
{
	if (b->fence != 0 && !gpuWait(b->fence, 1000)) {
		static uint32_t said;

		if (said++ < 8) {
			IOLog("SweetGPU: the GPU is not done with a buffer of %llu bytes: it stays its own\n", b->size);
		}
	} else {
		sweet_gpu_unmap(b->address, b->size);
		sweet_gpu_address_free(b->address, b->size);
		b->memory->release();
		OSSafeReleaseNULL(b->surface);
	}
	b->memory = NULL;
	b->surface = NULL;
}

IOReturn
SweetGPUUserClient::clientClose(void)
{
	if (fBuffers != NULL) {
		IOLockLock(gGPULock);
		for (uint32_t i = 0; i < GPU_BUFFERS; i++) {
			if (fBuffers[i].memory != NULL) {
				freeBuffer(&fBuffers[i]);
			}
		}
		IOLockUnlock(gGPULock);
	}
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
SweetGPUUserClient::free(void)
{
	if (fBuffers != NULL) {
		IODelete(fBuffers, struct gpu_buffer, GPU_BUFFERS);
		fBuffers = NULL;
	}
	IOUserClient::free();
}

/*
 * With the lock held: a handle and an address for `size` bytes of `memory`, its pages entered into
 * the GPU's address space. The buffer takes the caller's reference to the memory; NULL: none made
 * (the reference is still the caller's).
 */
struct gpu_buffer *
SweetGPUUserClient::newBuffer(IOMemoryDescriptor *memory, uint64_t size, uint32_t flags, uint32_t *handle)
{
	struct gpu_buffer *b = NULL;
	bool mapped = true;

	for (uint32_t n = 0; n < GPU_BUFFERS; n++) {
		uint32_t i = (fNextBuffer + n) % GPU_BUFFERS;

		if (fBuffers[i].memory == NULL) {
			b = &fBuffers[i];
			*handle = i + 1;
			fNextBuffer = i + 1;
			break;
		}
	}
	if (b == NULL || (b->address = sweet_gpu_address(size)) == 0) {
		IOLog("SweetGPU: no %s left for a buffer of %llu bytes\n", b ? "addresses" : "handles", size);
		return NULL;
	}
	for (uint64_t offset = 0; offset < size && mapped; offset += 4096) {
		IOByteCount length = 0;
		addr64_t phys = memory->getPhysicalSegment(offset, &length, kIOMemoryMapperNone);

		mapped = phys != 0 && length >= 4096 &&
		    sweet_gpu_map(b->address + offset, phys, (flags & MSM_BO_GPU_READONLY) != 0);
	}
	sweet_gpu_mapped();
	if (!mapped) {
		sweet_gpu_unmap(b->address, size);
		sweet_gpu_address_free(b->address, size);
		IOLog("SweetGPU: a buffer of %llu bytes cannot be entered into the GPU's address space\n", size);
		return NULL;
	}
	b->memory = memory;
	b->size = size;
	b->flags = flags;
	b->fence = 0;
	b->surface = NULL;
	b->kernel = NULL;
	b->seed = 0;
	b->pushed = 0;
	return b;
}

IOReturn
SweetGPUUserClient::gemNew(struct msm_gem_new *request)
{
	uint64_t size = round_page_64(request->size);
	IOBufferMemoryDescriptor *memory;
	struct gpu_buffer *b;

	if (request->size == 0 || request->size > GPU_BUFFER_MAX || size == 0 || size > GPU_BUFFER_MAX) {
		return kIOReturnBadArgument;
	}
	/* write-combined, as Linux maps freedreno's buffers: the program's writes go to memory, where the GPU reads */
	memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
	    kIODirectionInOut | kIOMemoryKernelUserShared | kIOMapWriteCombineCache, (vm_size_t)size, PAGE_SIZE);
	if (memory == NULL) {
		IOLog("SweetGPU: no memory for a buffer of %llu bytes\n", size);
		return kIOReturnNoMemory;
	}
	IOLockLock(gGPULock);
	b = newBuffer(memory, size, request->flags, &request->handle);
	IOLockUnlock(gGPULock);
	if (b == NULL) {
		memory->release();
		return kIOReturnNoMemory;
	}
	return kIOReturnSuccess;
}

/*
 * The memory of an IOSurface as a buffer, all of it from its first page (SweetSurface.cpp keeps the
 * pixels there); the surface is held for as long as the buffer is there. A surface that is already
 * a buffer of this client's is that buffer again, as with Linux's handles (freedreno asks under the
 * lock it gives buffers back under). Asked only to describe it (SURFACE_DESCRIBE), no buffer is made.
 */
IOReturn
SweetGPUUserClient::surfaceBuffer(struct sweet_surface_request *request)
{
	SweetSurface *s = sweet_surface_copy(request->surface);
	struct gpu_buffer *b = NULL;
	uint64_t size;

	if (s == NULL || s->memory == NULL) {
		OSSafeReleaseNULL(s);
		return kIOReturnNotFound;
	}
	size = round_page_64(s->allocSize);
	if (size == 0 || size > GPU_BUFFER_MAX || size > s->memory->getLength()) {
		s->release();
		return kIOReturnNoMemory;
	}
	request->size = size;
	request->offset = s->offset;
	request->rowBytes = s->bytesPerRow;
	request->width = s->width;
	request->height = s->height;
	request->pixelFormat = s->pixelFormat;
	request->planes = s->planeCount;
	if (request->handle == SURFACE_DESCRIBE) {
		request->handle = 0;
		s->release();
		return kIOReturnSuccess;
	}

	IOLockLock(gGPULock);
	for (uint32_t i = 0; i < GPU_BUFFERS; i++) {
		if (fBuffers[i].memory != NULL && fBuffers[i].surface == s) {
			b = &fBuffers[i];
			request->handle = i + 1;
			break;
		}
	}
	if (b != NULL) {
		s->release();                   /* (the buffer holds it already) */
	} else {
		s->memory->retain();
		if ((b = newBuffer(s->memory, size, 0, &request->handle)) == NULL) {
			s->memory->release();
			s->release();
		} else {
			b->surface = s;
			/* pushed out of the cache only where the kernel's and the programs' mappings are cached */
			b->kernel = OSDynamicCast(IOBufferMemoryDescriptor, s->memory) != NULL || s->wrapped ?
			    (void *)s->kernelAddress() : NULL;
		}
	}
	IOLockUnlock(gGPULock);
	return b != NULL ? kIOReturnSuccess : kIOReturnNoMemory;
}

IOReturn
SweetGPUUserClient::gemInfo(struct msm_gem_info *request)
{
	struct gpu_buffer *b;
	IOReturn ret = kIOReturnSuccess;

	IOLockLock(gGPULock);
	b = buffer(request->handle);
	if (b == NULL) {
		ret = kIOReturnBadArgument;
	} else if (request->info == MSM_INFO_GET_OFFSET) {
		request->value = (uint64_t)request->handle << 32;       /* nobody maps by it here */
	} else if (request->info == MSM_INFO_GET_IOVA) {
		request->value = b->address;    /* the buffer's until it is given back (freedreno asks once) */
	} else if (request->info == MSM_INFO_GET_FLAGS) {
		request->value = b->flags;
	} else if (request->info != MSM_INFO_SET_NAME) {
		ret = kIOReturnUnsupported;     /* SET_IOVA among them: addresses are the kernel's to give */
	}
	IOLockUnlock(gGPULock);
	return ret;
}

IOReturn
SweetGPUUserClient::submit(struct msm_gem_submit *request)
{
	struct msm_gem_submit_bo *bos = NULL;
	struct msm_gem_submit_cmd *cmds = NULL;
	uint64_t addresses[GPU_SUBMIT_CMDS];
	uint32_t words[GPU_SUBMIT_CMDS], count = 0, nr_bos = request->nr_bos, nr_cmds = request->nr_cmds;
	uint64_t began, again;
	IOReturn ret = kIOReturnBadArgument;

	if (nr_cmds == 0 || nr_cmds > GPU_SUBMIT_CMDS || nr_bos == 0 || nr_bos > GPU_SUBMIT_BOS) {
		IOLog("SweetGPU: a submission of %u command buffers in %u buffers: not taken\n", nr_cmds, nr_bos);
		return kIOReturnBadArgument;
	}
	bos = IONew(struct msm_gem_submit_bo, nr_bos);
	cmds = IONew(struct msm_gem_submit_cmd, nr_cmds);
	if (bos == NULL || cmds == NULL) {
		ret = kIOReturnNoMemory;
		goto out;
	}
	if (copyin((user_addr_t)request->bos, bos, nr_bos * sizeof(*bos)) != 0 ||
	    copyin((user_addr_t)request->cmds, cmds, nr_cmds * sizeof(*cmds)) != 0) {
		ret = kIOReturnVMError;
		goto out;
	}
	IOLockLock(gGPULock);
	if (gGPUStopped) {
		IOLockUnlock(gGPULock);
		ret = kIOReturnNotReady;
		goto out;
	}
	/*
	 * What this processor's cache holds of an IOSurface the GPU is to read or write (somebody drew
	 * into it with this processor, or read it, as the display does) goes out of the cache now: when
	 * its seed says it has changed, and in any case once in a few milliseconds, which is once a frame
	 * rather than once a submission (not everybody who writes a surface says so). After HackiOS.
	 */
	began = mach_absolute_time();
	nanoseconds_to_absolutetime(GPU_PUSH_AGAIN_MS * NSEC_PER_MSEC, &again);
	for (uint32_t i = 0; i < nr_bos; i++) {
		struct gpu_buffer *b = buffer(bos[i].handle);
		uint32_t seed;

		if (b == NULL || b->surface == NULL || b->kernel == NULL) {
			continue;
		}
		seed = b->surface->state != NULL ? *(volatile uint32_t *)((uint8_t *)b->surface->state->getBytesNoCopy() + 0xc) : 0;
		if (b->pushed != 0 && seed == b->seed && began - b->pushed < again) {
			continue;
		}
		b->seed = seed;                 /* (SWEET_SURFACE_STATE_SEED in SweetSurface.cpp) */
		b->pushed = began;
		sweet_gpu_to_memory(b->kernel, b->size);
	}
	sweet_gpu_to_memory(NULL, 0);           /* what the program wrote last is in memory (a barrier) */
	for (uint32_t i = 0; i < nr_cmds; i++) {
		struct gpu_buffer *b = cmds[i].submit_idx < nr_bos ? buffer(bos[cmds[i].submit_idx].handle) : NULL;

		if (cmds[i].type != MSM_SUBMIT_CMD_BUF) {
			continue;                       /* the other kinds are for a GPU shared between address spaces */
		}
		if (b == NULL || cmds[i].nr_relocs != 0 || (cmds[i].size & 3) || (cmds[i].submit_offset & 3) ||
		    (uint64_t)cmds[i].submit_offset + cmds[i].size > b->size) {
			IOLog("SweetGPU: command buffer %u of a submission: buffer %u, offset %u, %u bytes, %u relocations: not taken\n",
			    i, cmds[i].submit_idx, cmds[i].submit_offset, cmds[i].size, cmds[i].nr_relocs);
			IOLockUnlock(gGPULock);
			goto out;
		}
		addresses[count] = b->address + cmds[i].submit_offset;
		words[count++] = cmds[i].size / 4;
	}
	request->fence = (count != 0 && !gGPUStopped) ? sweet_gpu_submit(addresses, words, count) : 0;
	if (request->fence == 0 && count != 0 && !gGPUStopped) {
		sweet_gpu_report("the ring stays full");
		gGPUStopped = true;
	}
	if (request->flags & MSM_SUBMIT_FENCE_FD_OUT) {
		request->fence_fd = -1;
	}
	for (uint32_t i = 0; request->fence != 0 && i < nr_bos; i++) {
		struct gpu_buffer *b = buffer(bos[i].handle);

		if (b != NULL) {
			b->fence = request->fence;
		}
		/* a surface the GPU is to write: the display waits for that before it copies it */
		if (b != NULL && b->surface != NULL && (bos[i].flags & MSM_SUBMIT_BO_WRITE)) {
			uint32_t at = GPU_SURFACE_WRITES;

			for (uint32_t k = 0; k < GPU_SURFACE_WRITES; k++) {
				if (gSurfaceWrites[k].id == b->surface->id || (at == GPU_SURFACE_WRITES && gSurfaceWrites[k].id == 0)) {
					at = k;
				}
			}
			if (at == GPU_SURFACE_WRITES) {
				at = gSurfaceWriteNext++ % GPU_SURFACE_WRITES;
			}
			gSurfaceWrites[at].id = b->surface->id;
			gSurfaceWrites[at].fence = request->fence;
		}
	}
	if (request->fence != 0 && fSubmits++ < 4) {
		IOLog("SweetGPU: submission %u: %u command buffers, the first at %#llx, %u words\n", request->fence, count,
		    addresses[0], words[0]);
	}
	IOLockUnlock(gGPULock);
	ret = request->fence != 0 ? kIOReturnSuccess : kIOReturnTimeout;
out:
	if (bos != NULL) {
		IODelete(bos, struct msm_gem_submit_bo, nr_bos);
	}
	if (cmds != NULL) {
		IODelete(cmds, struct msm_gem_submit_cmd, nr_cmds);
	}
	return ret;
}

/*
 * For the display (SweetFramebuffer.cpp), before it copies surface `id` (whose pixels are `kernel`,
 * `size` bytes, in the kernel's cached mapping): false while the GPU has yet to finish what it was
 * last told to draw into it; when it has, what this processor's cache holds of the surface (it may
 * have read the surface before) goes, so the copy reads what the GPU wrote. Without a GPU, true.
 */
bool
sweet_gpu_surface_ready(uint32_t id, void *kernel, uint64_t size)
{
	uint32_t fence = 0;
	bool drawn = false;

	if (gGPULock == NULL || id == 0) {
		return true;
	}
	IOLockLock(gGPULock);
	/* A busy surface may have aged out of the small association cache. In that
	 * case wait for all submitted work; never infer completion from eviction. */
	fence = sweet_gpu_last();
	if (gGPUStopped) {
		IOLockUnlock(gGPULock);
		return false;
	}
	for (uint32_t k = 0; k < GPU_SURFACE_WRITES; k++) {
		if (gSurfaceWrites[k].id == id) {
			fence = gSurfaceWrites[k].fence;
			if (fence == 0 || sweet_gpu_done(fence)) {
				gSurfaceWrites[k].id = 0;
				fence = 0;
				drawn = true;
			}
			break;
		}
	}
	if (fence != 0 && sweet_gpu_done(fence)) { fence = 0; drawn = true; }
	IOLockUnlock(gGPULock);
	if (fence != 0) {
		return false;
	}
	if (drawn && kernel != NULL && size != 0) {
		sweet_gpu_to_memory(kernel, size);
	}
	return true;
}

IOReturn
SweetGPUUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory)
{
	struct gpu_buffer *b;
	IOReturn ret = kIOReturnBadArgument;

	IOLockLock(gGPULock);
	b = buffer(type);
	if (b != NULL) {
		b->memory->retain();
		*memory = b->memory;
		*options = 0;
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gGPULock);
	return ret;
}

IOReturn
SweetGPUUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target, __unused void *reference)
{
	union {
		struct msm_param param;
		struct msm_gem_new gemNew;
		struct msm_gem_info gemInfo;
		struct msm_gem_cpu_prep cpuPrep;
		struct msm_gem_submit submit;
		struct msm_wait_fence waitFence;
		struct msm_gem_madvise madvise;
		struct msm_submitqueue queue;
		struct msm_submitqueue_query query;
		struct drm_gem_close close;
		struct sweet_surface_request surface;
	} r;
	size_t size = args->structureInputSize;
	struct gpu_buffer *b;
	IOReturn ret = kIOReturnSuccess;
	uint64_t zero = 0, patience;
	uint32_t fence;

	if (args->structureInput == NULL || size > sizeof(r) || fBuffers == NULL) {
		return kIOReturnBadArgument;
	}
	size_t required = 0;
	switch (selector) {
	case kMsmGetParam: required = sizeof(r.param); break;
	case kMsmGemNew: required = sizeof(r.gemNew); break;
	case kMsmGemInfo: required = sizeof(r.gemInfo); break;
	case kMsmGemSubmit: required = sizeof(r.submit); break;
	case kMsmWaitFence: required = sizeof(r.waitFence); break;
	case kMsmGemCpuPrep: required = sizeof(r.cpuPrep); break;
	case kSweetGpuSurface: required = sizeof(r.surface); break;
	case kDrmGemClose: required = sizeof(r.close); break;
	default: break;
	}
	if (size < required) return kIOReturnBadArgument;
	if (!sweet_gpu_ready()) {
		return kIOReturnNotReady;
	}
	bzero(&r, sizeof(r));
	memcpy(&r, args->structureInput, size);

	switch (selector) {
	case kMsmGetParam:
		IOLockLock(gGPULock);
		ret = sweet_gpu_parameter(r.param.param, &r.param.value) ? kIOReturnSuccess : kIOReturnBadArgument;
		IOLockUnlock(gGPULock);
		break;
	case kMsmSetParam:                      /* the program's name, for Linux's lists of who uses the GPU */
	case kMsmGemCpuFini:
	case kMsmSubmitQueueClose:
		break;
	case kMsmGemNew:
		ret = gemNew(&r.gemNew);
		break;
	case kMsmGemInfo:
		ret = gemInfo(&r.gemInfo);
		break;
	case kMsmGemCpuPrep:                    /* is the GPU done with a buffer (the last submission naming it) */
		IOLockLock(gGPULock);
		b = buffer(r.cpuPrep.handle);
		fence = b != NULL ? b->fence : sweet_gpu_last();
		patience = (r.cpuPrep.op & MSM_PREP_NOSYNC) ? 0 : millisecondsUntil(&r.cpuPrep.timeout);
		if (fence != 0 && !gpuWait(fence, patience)) {
			ret = kIOReturnBusy;
		}
		IOLockUnlock(gGPULock);
		break;
	case kMsmGemSubmit:
		ret = submit(&r.submit);
		break;
	case kMsmWaitFence:
		IOLockLock(gGPULock);
		if (!gpuWait(r.waitFence.fence, millisecondsUntil(&r.waitFence.timeout))) {
			ret = kIOReturnTimeout;
		}
		IOLockUnlock(gGPULock);
		break;
	case kMsmGemMadvise:
		r.madvise.retained = 1;
		break;
	case kMsmSubmitQueueNew:
		r.queue.id = ++fNextQueue;
		break;
	case kMsmSubmitQueueQuery:              /* how often the GPU has faulted on this client's commands */
		if (r.query.data != 0 && r.query.len >= sizeof(zero)) {
			(void)copyout(&zero, (user_addr_t)r.query.data, sizeof(zero));
		}
		break;
	case kSweetGpuSurface:
		ret = surfaceBuffer(&r.surface);
		break;
	case kDrmGemClose:
		IOLockLock(gGPULock);
		b = buffer(r.close.handle);
		if (b == NULL) {
			ret = kIOReturnBadArgument;
		} else {
			freeBuffer(b);
		}
		IOLockUnlock(gGPULock);
		break;
	default:
		IOLog("SweetGPU: request %#x (%zu bytes) is not known here\n", selector, size);
		ret = kIOReturnUnsupported;
		break;
	}
	if (ret == kIOReturnSuccess && args->structureOutput != NULL) {
		if (args->structureOutputSize > size) {
			args->structureOutputSize = (uint32_t)size;
		}
		memcpy(args->structureOutput, &r, args->structureOutputSize);
	}
	return ret;
}
#endif /* ARM64_BOARD_CONFIG_SWEET */

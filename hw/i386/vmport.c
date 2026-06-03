/*
 * QEMU VMPort emulation
 *
 * Copyright (C) 2007 Hervé Poussineau
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/*
 * Guest code that interacts with this virtual device can be found
 * in VMware open-vm-tools open-source project:
 * https://github.com/vmware/open-vm-tools
 */

// #define VERBOSE
// #define EXP3D
// #define EXPCAPS

#include "qemu/osdep.h"
#include "hw/isa/isa.h"
#include "hw/i386/vmport.h"
#include "hw/qdev-properties.h"
#include "hw/boards.h"
#include "sysemu/sysemu.h"
#include "sysemu/hw_accel.h"
#include "sysemu/qtest.h"
#include "qemu/log.h"
#include "trace.h"
#include "qom/object.h"

#define VMPORT_MAGIC   0x564D5868

/* Compatibility flags for migration */
#define VMPORT_COMPAT_READ_SET_EAX_BIT              0
#define VMPORT_COMPAT_SIGNAL_UNSUPPORTED_CMD_BIT    1
#define VMPORT_COMPAT_REPORT_VMX_TYPE_BIT           2
#define VMPORT_COMPAT_CMDS_V2_BIT                   3
#define VMPORT_COMPAT_READ_SET_EAX              \
    (1 << VMPORT_COMPAT_READ_SET_EAX_BIT)
#define VMPORT_COMPAT_SIGNAL_UNSUPPORTED_CMD    \
    (1 << VMPORT_COMPAT_SIGNAL_UNSUPPORTED_CMD_BIT)
#define VMPORT_COMPAT_REPORT_VMX_TYPE           \
    (1 << VMPORT_COMPAT_REPORT_VMX_TYPE_BIT)
#define VMPORT_COMPAT_CMDS_V2                   \
    (1 << VMPORT_COMPAT_CMDS_V2_BIT)

/* vCPU features reported by CMD_GET_VCPU_INFO */
#define VCPU_INFO_SLC64_BIT             0
#define VCPU_INFO_SYNC_VTSCS_BIT        1
#define VCPU_INFO_HV_REPLAY_OK_BIT      2
#define VCPU_INFO_LEGACY_X2APIC_BIT     3
#define VCPU_INFO_RESERVED_BIT          31

#ifdef VERBOSE
#define VPRINT(fmt, ...)                                                       \
  printf("vmport (%s): %u - %s: " fmt, __FILE__, (uint32_t)time(NULL),         \
         __func__, ##__VA_ARGS__)
#else
#define VPRINT(...)
#endif

OBJECT_DECLARE_SIMPLE_TYPE(VMPortState, VMPORT)

struct VMPortState {
    ISADevice parent_obj;

    MemoryRegion io;
    VMPortReadFunc *func[VMPORT_CMD_MAX];
    void *opaque[VMPORT_CMD_MAX];

    uint32_t vmware_vmx_version;
    uint8_t vmware_vmx_type;

    uint32_t compat_flags;
};

static VMPortState *port_state;

bool vmport_register(VMPortCommand command, VMPortReadFunc *func, void *opaque)
{
    if (command >= VMPORT_CMD_MAX || !port_state) {
        return false;
    }

    trace_vmport_register(command, func, opaque);
    port_state->func[command] = func;
    port_state->opaque[command] = opaque;
    return true;
}

static uint64_t vmport_ioport_read(void *opaque, hwaddr addr,
                                   unsigned size)
{
    VMPortState *s = opaque;
    CPUState *cs = current_cpu;
    X86CPU *cpu = X86_CPU(cs);
    CPUX86State *env;
    unsigned char command;
    uint32_t eax;

    if (qtest_enabled()) {
        return -1;
    }
    env = &cpu->env;
    cpu_synchronize_state(cs);

    eax = env->regs[R_EAX];
    if (eax != VMPORT_MAGIC) {
        goto err;
    }

    command = env->regs[R_ECX];
    trace_vmport_command(command);
    if (command >= VMPORT_CMD_MAX || !s->func[command]) {
        qemu_log_mask(LOG_UNIMP, "vmport: unknown command %x\n", command);
        goto err;
    }

    eax = s->func[command](s->opaque[command], addr);
    goto out;

err:
    if (s->compat_flags & VMPORT_COMPAT_SIGNAL_UNSUPPORTED_CMD) {
        eax = UINT32_MAX;
    }

out:
    /*
     * The call above to cpu_synchronize_state() gets vCPU registers values
     * to QEMU but also cause QEMU to write QEMU vCPU registers values to
     * vCPU implementation (e.g. Accelerator such as KVM) just before
     * resuming guest.
     *
     * Therefore, in order to make IOPort return value propagate to
     * guest EAX, we need to explicitly update QEMU EAX register value.
     */
    if (s->compat_flags & VMPORT_COMPAT_READ_SET_EAX) {
        cpu->env.regs[R_EAX] = eax;
    }

    return eax;
}

static void vmport_ioport_write(void *opaque, hwaddr addr,
                                uint64_t val, unsigned size)
{
    X86CPU *cpu = X86_CPU(current_cpu);

    if (qtest_enabled()) {
        return;
    }
    cpu->env.regs[R_EAX] = vmport_ioport_read(opaque, addr, 4);
}

static uint32_t vmport_cmd_getversion(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_getversion was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);

    if (qtest_enabled()) {
        return -1;
    }
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    if (port_state->compat_flags & VMPORT_COMPAT_REPORT_VMX_TYPE) {
        cpu->env.regs[R_ECX] = port_state->vmware_vmx_type;
    }
    return port_state->vmware_vmx_version;
}

static uint32_t vmport_cmd_getuuid(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_getuuid was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    uint32_t *uuid_parts = (uint32_t *)(qemu_uuid.data);

    cpu->env.regs[R_EAX] = le32_to_cpu(uuid_parts[0]);
    cpu->env.regs[R_EBX] = le32_to_cpu(uuid_parts[1]);
    cpu->env.regs[R_ECX] = le32_to_cpu(uuid_parts[2]);
    cpu->env.regs[R_EDX] = le32_to_cpu(uuid_parts[3]);
    return cpu->env.regs[R_EAX];
}

static uint32_t vmport_cmd_getmemsize(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_getmemsize was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);

    if (qtest_enabled()) {
        return -1;
    }
    cpu->env.regs[R_EBX] = 0x1177;
    return current_machine->ram_size >> 20; /* in MB */
}

static uint32_t vmport_cmd_gethz(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_gethz was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);

    if (cpu->env.tsc_khz && cpu->env.apic_bus_freq) {
        uint64_t tsc_freq = (uint64_t)cpu->env.tsc_khz * 1000;

        cpu->env.regs[R_ECX] = cpu->env.apic_bus_freq;
        cpu->env.regs[R_EBX] = (uint32_t)(tsc_freq >> 32);
        cpu->env.regs[R_EAX] = (uint32_t)tsc_freq;
    } else {
        /* Signal cmd as not supported */
        cpu->env.regs[R_EBX] = UINT32_MAX;
    }

    return cpu->env.regs[R_EAX];
}

static uint32_t vmport_cmd_get_vcpu_info(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_get_vcpu_info was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    uint32_t ret = 0;

    if (cpu->env.features[FEAT_1_ECX] & CPUID_EXT_X2APIC) {
        ret |= 1 << VCPU_INFO_LEGACY_X2APIC_BIT;
    }

    return ret;
}
static uint32_t vmport_cmd_abspointer_command(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_abspointer_command was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_abspointer_data(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_abspointer_data was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_abspointer_restrict(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_abspointer_restrict was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_abspointer_status(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_abspointer_status was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_acpi_hotplug_cbret(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_acpi_hotplug_cbret was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_acpi_hotplug_cpu(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_acpi_hotplug_cpu was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_acpi_hotplug_device(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_acpi_hotplug_device was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_acpi_hotplug_memory(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_acpi_hotplug_memory was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_apmfunction(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_apmfunction was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_apple_gpu_res_set(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_apple_gpu_res_set was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_biosbbs(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_biosbbs was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_bug328986(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_bug328986 was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_checkforcebiossetup(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_checkforcebiossetup was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_copy_physmem(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_copy_physmem was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_coredump_unsync(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_coredump_unsync was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_debugevent(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_debugevent was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_devel_fakehardware(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_devel_fakehardware was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_efi_boot_config(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_efi_boot_config was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_efi_serialcon_config(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_efi_serialcon_config was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_enable_msg(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_enable_msg was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_enable_usb_mouse(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_enable_usb_mouse was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_firmware_acpi_services(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_firmware_acpi_services was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_firmware_error(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_firmware_error was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_firmware_init(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_firmware_init was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_firmware_update(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_firmware_update was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_fuzzer_helper(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_fuzzer_helper was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_getbuildnum(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_getbuildnum was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_getdevicelistelement(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_getdevicelistelement was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_getdiskgeo(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_getdiskgeo was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_getentropy(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_getentropy was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_getguioptions(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_getguioptions was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_gethwversion(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_gethwversion was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_getmhz(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_getmhz was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_getnextpiece(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_getnextpiece was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_getptrlocation(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_getptrlocation was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_getscreensize(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_getscreensize was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_getsellength(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_getsellength was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_gettime(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_gettime was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_gettimefull(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_gettimefull was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_gettimefull_with_lag(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_gettimefull_with_lag was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_get_force_x2apic(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_get_force_x2apic was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_get_host_video_modes(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_get_host_video_modes was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_get_hw_model(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_get_hw_model was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_get_pci_bar(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_get_pci_bar was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_get_pci_hole(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_get_pci_hole was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_gmm(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_gmm was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_goto_tcl(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_goto_tcl was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_guest_integrity(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_guest_integrity was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_guest_page_hints(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_guest_page_hints was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_hostcopy(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_hostcopy was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_initpcioprom(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_initpcioprom was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_inject_key(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_inject_key was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_inject_mouse(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_inject_mouse was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_int13(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_int13 was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_isacpidisabled(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_isacpidisabled was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_isgosdarwin(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_isgosdarwin was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_ismouseabsolute(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_ismouseabsolute was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_lazytimeremulation(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_lazytimeremulation was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_mapmem(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_mapmem was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_max(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_max was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_message(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_message was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_mkstest(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_mkstest was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_mks_guest_stats(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_mks_guest_stats was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_monitor_control(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_monitor_control was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_nesting_control(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_nesting_control was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_osnotfound(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_osnotfound was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_osnotmacosxserver(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_osnotmacosxserver was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_patch_acpi_tables(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_patch_acpi_tables was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_patch_smbios_structs(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_patch_smbios_structs was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_precisionclock(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_precisionclock was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_putchr(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_putchr was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_putchr12(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_putchr12 was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_read_debug_file(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_read_debug_file was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_reportguestcrash(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_reportguestcrash was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_screenshot(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_screenshot was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_secureboot(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_secureboot was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_sendpsharehints(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_sendpsharehints was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_service_vm(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_service_vm was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_setguioptions(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_setguioptions was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_setnextpiece(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_setnextpiece was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_setptrlocation(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_setptrlocation was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_setsellength(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_setsellength was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_set_pci_hole(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_set_pci_hole was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_sgdt(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_sgdt was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_should_generate_systemid(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_should_generate_systemid was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_sidt(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_sidt was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_sldt_str(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_sldt_str was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_statelogger(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_statelogger was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_stealclock(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_stealclock was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_stopcatchup(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_stopcatchup was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_timer_sponge(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_timer_sponge was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_toe(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_toe was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_toggledevice(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_toggledevice was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_usb_hotplug_mouse(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_usb_hotplug_mouse was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_vassert(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_vassert was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_vmk_info(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_vmk_info was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

static uint32_t vmport_cmd_xpmode(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_xpmode was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    cpu->env.regs[R_EAX] = -2;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = -2;
    cpu->env.regs[R_EDX] = -2;
    return -2;
}

/*

typedef enum {
	SVGABackdoorCapDeviceCaps = 0,
	SVGABackdoorCapFifoCaps = 1,
	SVGABackdoorCap3dHWVersion = 2,
	SVGABackdoorCapDeviceCaps2 = 3,
	SVGABackdoorCapDevelCaps = 4,
	SVGABackdoorCapDevCaps = 5,
	SVGABackdoorDevelRenderer = 6,
	SVGABackdoorDevelUsingISB = 7,
	SVGABackdoorCapMax = 8,
} SVGABackdoorCapType;

*/

static uint32_t vmport_cmd_get_svga_capabilities(void *opaque, uint32_t addr)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    VPRINT("vmport_cmd_get_svga_capabilities was just executed: eax=0x%lx, ebx=0x%lx, ecx=0x%lx, edx=0x%lx\n", cpu->env.regs[R_EAX], cpu->env.regs[R_EBX], cpu->env.regs[R_ECX], cpu->env.regs[R_EDX]);
    uint32_t ret = 0;
    switch ((cpu->env.regs[R_ECX] >> 16) & 0xffff) {
        case 0: // SVGABackdoorCapDeviceCaps
            ret = 0xffffffff;
            #ifndef EXPCAPS
            ret -= 0x00000001; // SVGA_CAP_UNKNOWN_A (Windows 9x)
            ret -= 0x00000002; // SVGA_CAP_RECT_COPY (Windows 9x & Windows (XPDM))
            ret -= 0x00000008; // SVGA_CAP_UNKNOWN_C (Windows 9x)
            #endif
            #ifndef EXP3D
            ret -= 0x00004000; // SVGA_CAP_3D (Windows (WDDM))
            #endif
            #ifndef EXPCAPS
            ret -= 0x00800000; // SVGA_CAP_SCREEN_OBJECT_2 (Linux)
            ret -= 0x04000000; // SVGA_CAP_CMD_BUFFERS_2 (Windows (WDDM))
            ret -= 0x08000000; // SVGA_CAP_GBOBJECTS (Linux, Windows (XPDM) & Windows (WDDM))
            #endif
            break;
        case 1: // SVGABackdoorCapFifoCaps
            ret = 0xffffffff;
            #ifndef EXPCAPS
            ret -= 0x00000080; // SVGA_FIFO_CAP_SCREEN_OBJECT
            ret -= 0x00000200; // SVGA_FIFO_CAP_SCREEN_OBJECT_2
            #endif
            break;
        case 2: // SVGABackdoorCap3dHWVersion
            ret = 0x00020001; // SVGA3D_HWVERSION_WS8_B1
            break;
        default:
            ret = -1;
            break;
    }
    cpu->env.regs[R_EAX] = ret;
    cpu->env.regs[R_EBX] = VMPORT_MAGIC;
    cpu->env.regs[R_ECX] = 0;
    cpu->env.regs[R_EDX] = 0;
    return ret;
}

static const MemoryRegionOps vmport_ops = {
    .read = vmport_ioport_read,
    .write = vmport_ioport_write,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void vmport_realizefn(DeviceState *dev, Error **errp)
{
    ISADevice *isadev = ISA_DEVICE(dev);
    VMPortState *s = VMPORT(dev);

    memory_region_init_io(&s->io, OBJECT(s), &vmport_ops, s, "vmport", 1);
    isa_register_ioport(isadev, &s->io, 0x5658);

    port_state = s;

    /* Register some generic port commands */
    vmport_register(VMPORT_CMD_ABSPOINTER_COMMAND, vmport_cmd_abspointer_command, NULL);
    vmport_register(VMPORT_CMD_ABSPOINTER_DATA, vmport_cmd_abspointer_data, NULL);
    vmport_register(VMPORT_CMD_ABSPOINTER_RESTRICT, vmport_cmd_abspointer_restrict, NULL);
    vmport_register(VMPORT_CMD_ABSPOINTER_STATUS, vmport_cmd_abspointer_status, NULL);
    vmport_register(VMPORT_CMD_ACPI_HOTPLUG_CBRET, vmport_cmd_acpi_hotplug_cbret, NULL);
    vmport_register(VMPORT_CMD_ACPI_HOTPLUG_CPU, vmport_cmd_acpi_hotplug_cpu, NULL);
    vmport_register(VMPORT_CMD_ACPI_HOTPLUG_DEVICE, vmport_cmd_acpi_hotplug_device, NULL);
    vmport_register(VMPORT_CMD_ACPI_HOTPLUG_MEMORY, vmport_cmd_acpi_hotplug_memory, NULL);
    vmport_register(VMPORT_CMD_APMFUNCTION, vmport_cmd_apmfunction, NULL);
    vmport_register(VMPORT_CMD_APPLE_GPU_RES_SET, vmport_cmd_apple_gpu_res_set, NULL);
    vmport_register(VMPORT_CMD_BIOSBBS, vmport_cmd_biosbbs, NULL);
    vmport_register(VMPORT_CMD_BUG328986, vmport_cmd_bug328986, NULL);
    vmport_register(VMPORT_CMD_CHECKFORCEBIOSSETUP, vmport_cmd_checkforcebiossetup, NULL);
    vmport_register(VMPORT_CMD_COPY_PHYSMEM, vmport_cmd_copy_physmem, NULL);
    vmport_register(VMPORT_CMD_COREDUMP_UNSYNC, vmport_cmd_coredump_unsync, NULL);
    vmport_register(VMPORT_CMD_DEBUGEVENT, vmport_cmd_debugevent, NULL);
    vmport_register(VMPORT_CMD_DEVEL_FAKEHARDWARE, vmport_cmd_devel_fakehardware, NULL);
    vmport_register(VMPORT_CMD_EFI_BOOT_CONFIG, vmport_cmd_efi_boot_config, NULL);
    vmport_register(VMPORT_CMD_EFI_SERIALCON_CONFIG, vmport_cmd_efi_serialcon_config, NULL);
    vmport_register(VMPORT_CMD_ENABLE_MSG, vmport_cmd_enable_msg, NULL);
    vmport_register(VMPORT_CMD_ENABLE_USB_MOUSE, vmport_cmd_enable_usb_mouse, NULL);
    vmport_register(VMPORT_CMD_FIRMWARE_ACPI_SERVICES, vmport_cmd_firmware_acpi_services, NULL);
    vmport_register(VMPORT_CMD_FIRMWARE_ERROR, vmport_cmd_firmware_error, NULL);
    vmport_register(VMPORT_CMD_FIRMWARE_INIT, vmport_cmd_firmware_init, NULL);
    vmport_register(VMPORT_CMD_FIRMWARE_UPDATE, vmport_cmd_firmware_update, NULL);
    vmport_register(VMPORT_CMD_FUZZER_HELPER, vmport_cmd_fuzzer_helper, NULL);
    vmport_register(VMPORT_CMD_GETBUILDNUM, vmport_cmd_getbuildnum, NULL);
    vmport_register(VMPORT_CMD_GETDEVICELISTELEMENT, vmport_cmd_getdevicelistelement, NULL);
    vmport_register(VMPORT_CMD_GETDISKGEO, vmport_cmd_getdiskgeo, NULL);
    vmport_register(VMPORT_CMD_GETENTROPY, vmport_cmd_getentropy, NULL);
    vmport_register(VMPORT_CMD_GETGUIOPTIONS, vmport_cmd_getguioptions, NULL);
    vmport_register(VMPORT_CMD_GETHWVERSION, vmport_cmd_gethwversion, NULL);
    vmport_register(VMPORT_CMD_GETHZ, vmport_cmd_gethz, NULL);
    vmport_register(VMPORT_CMD_GETMEMSIZE, vmport_cmd_getmemsize, NULL);
    vmport_register(VMPORT_CMD_GETMHZ, vmport_cmd_getmhz, NULL);
    vmport_register(VMPORT_CMD_GETNEXTPIECE, vmport_cmd_getnextpiece, NULL);
    vmport_register(VMPORT_CMD_GETPTRLOCATION, vmport_cmd_getptrlocation, NULL);
    vmport_register(VMPORT_CMD_GETSCREENSIZE, vmport_cmd_getscreensize, NULL);
    vmport_register(VMPORT_CMD_GETSELLENGTH, vmport_cmd_getsellength, NULL);
    vmport_register(VMPORT_CMD_GETTIME, vmport_cmd_gettime, NULL);
    vmport_register(VMPORT_CMD_GETTIMEFULL, vmport_cmd_gettimefull, NULL);
    vmport_register(VMPORT_CMD_GETTIMEFULL_WITH_LAG, vmport_cmd_gettimefull_with_lag, NULL);
    vmport_register(VMPORT_CMD_GETUUID, vmport_cmd_getuuid, NULL);
    vmport_register(VMPORT_CMD_GETVERSION, vmport_cmd_getversion, NULL);
    vmport_register(VMPORT_CMD_GET_FORCE_X2APIC, vmport_cmd_get_force_x2apic, NULL);
    vmport_register(VMPORT_CMD_GET_HOST_VIDEO_MODES, vmport_cmd_get_host_video_modes, NULL);
    vmport_register(VMPORT_CMD_GET_HW_MODEL, vmport_cmd_get_hw_model, NULL);
    vmport_register(VMPORT_CMD_GET_PCI_BAR, vmport_cmd_get_pci_bar, NULL);
    vmport_register(VMPORT_CMD_GET_PCI_HOLE, vmport_cmd_get_pci_hole, NULL);
    vmport_register(VMPORT_CMD_GET_SVGA_CAPABILITIES, vmport_cmd_get_svga_capabilities, NULL);
    vmport_register(VMPORT_CMD_GET_VCPU_INFO, vmport_cmd_get_vcpu_info, NULL);
    vmport_register(VMPORT_CMD_GMM, vmport_cmd_gmm, NULL);
    vmport_register(VMPORT_CMD_GOTO_TCL, vmport_cmd_goto_tcl, NULL);
    vmport_register(VMPORT_CMD_GUEST_INTEGRITY, vmport_cmd_guest_integrity, NULL);
    vmport_register(VMPORT_CMD_GUEST_PAGE_HINTS, vmport_cmd_guest_page_hints, NULL);
    vmport_register(VMPORT_CMD_HOSTCOPY, vmport_cmd_hostcopy, NULL);
    vmport_register(VMPORT_CMD_INITPCIOPROM, vmport_cmd_initpcioprom, NULL);
    vmport_register(VMPORT_CMD_INJECT_KEY, vmport_cmd_inject_key, NULL);
    vmport_register(VMPORT_CMD_INJECT_MOUSE, vmport_cmd_inject_mouse, NULL);
    vmport_register(VMPORT_CMD_INT13, vmport_cmd_int13, NULL);
    vmport_register(VMPORT_CMD_ISACPIDISABLED, vmport_cmd_isacpidisabled, NULL);
    vmport_register(VMPORT_CMD_ISGOSDARWIN, vmport_cmd_isgosdarwin, NULL);
    vmport_register(VMPORT_CMD_ISMOUSEABSOLUTE, vmport_cmd_ismouseabsolute, NULL);
    vmport_register(VMPORT_CMD_LAZYTIMEREMULATION, vmport_cmd_lazytimeremulation, NULL);
    vmport_register(VMPORT_CMD_MAPMEM, vmport_cmd_mapmem, NULL);
    vmport_register(VMPORT_CMD_MAX, vmport_cmd_max, NULL);
    vmport_register(VMPORT_CMD_MESSAGE, vmport_cmd_message, NULL);
    vmport_register(VMPORT_CMD_MKSTEST, vmport_cmd_mkstest, NULL);
    vmport_register(VMPORT_CMD_MKS_GUEST_STATS, vmport_cmd_mks_guest_stats, NULL);
    vmport_register(VMPORT_CMD_MONITOR_CONTROL, vmport_cmd_monitor_control, NULL);
    vmport_register(VMPORT_CMD_NESTING_CONTROL, vmport_cmd_nesting_control, NULL);
    vmport_register(VMPORT_CMD_OSNOTFOUND, vmport_cmd_osnotfound, NULL);
    vmport_register(VMPORT_CMD_OSNOTMACOSXSERVER, vmport_cmd_osnotmacosxserver, NULL);
    vmport_register(VMPORT_CMD_PATCH_ACPI_TABLES, vmport_cmd_patch_acpi_tables, NULL);
    vmport_register(VMPORT_CMD_PATCH_SMBIOS_STRUCTS, vmport_cmd_patch_smbios_structs, NULL);
    vmport_register(VMPORT_CMD_PRECISIONCLOCK, vmport_cmd_precisionclock, NULL);
    vmport_register(VMPORT_CMD_PUTCHR, vmport_cmd_putchr, NULL);
    vmport_register(VMPORT_CMD_PUTCHR12, vmport_cmd_putchr12, NULL);
    vmport_register(VMPORT_CMD_READ_DEBUG_FILE, vmport_cmd_read_debug_file, NULL);
    vmport_register(VMPORT_CMD_REPORTGUESTCRASH, vmport_cmd_reportguestcrash, NULL);
    vmport_register(VMPORT_CMD_SCREENSHOT, vmport_cmd_screenshot, NULL);
    vmport_register(VMPORT_CMD_SECUREBOOT, vmport_cmd_secureboot, NULL);
    vmport_register(VMPORT_CMD_SENDPSHAREHINTS, vmport_cmd_sendpsharehints, NULL);
    vmport_register(VMPORT_CMD_SERVICE_VM, vmport_cmd_service_vm, NULL);
    vmport_register(VMPORT_CMD_SETGUIOPTIONS, vmport_cmd_setguioptions, NULL);
    vmport_register(VMPORT_CMD_SETNEXTPIECE, vmport_cmd_setnextpiece, NULL);
    vmport_register(VMPORT_CMD_SETPTRLOCATION, vmport_cmd_setptrlocation, NULL);
    vmport_register(VMPORT_CMD_SETSELLENGTH, vmport_cmd_setsellength, NULL);
    vmport_register(VMPORT_CMD_SET_PCI_HOLE, vmport_cmd_set_pci_hole, NULL);
    vmport_register(VMPORT_CMD_SGDT, vmport_cmd_sgdt, NULL);
    vmport_register(VMPORT_CMD_SHOULD_GENERATE_SYSTEMID, vmport_cmd_should_generate_systemid, NULL);
    vmport_register(VMPORT_CMD_SIDT, vmport_cmd_sidt, NULL);
    vmport_register(VMPORT_CMD_SLDT_STR, vmport_cmd_sldt_str, NULL);
    vmport_register(VMPORT_CMD_STATELOGGER, vmport_cmd_statelogger, NULL);
    vmport_register(VMPORT_CMD_STEALCLOCK, vmport_cmd_stealclock, NULL);
    vmport_register(VMPORT_CMD_STOPCATCHUP, vmport_cmd_stopcatchup, NULL);
    vmport_register(VMPORT_CMD_TIMER_SPONGE, vmport_cmd_timer_sponge, NULL);
    vmport_register(VMPORT_CMD_TOE, vmport_cmd_toe, NULL);
    vmport_register(VMPORT_CMD_TOGGLEDEVICE, vmport_cmd_toggledevice, NULL);
    vmport_register(VMPORT_CMD_USB_HOTPLUG_MOUSE, vmport_cmd_usb_hotplug_mouse, NULL);
    vmport_register(VMPORT_CMD_VASSERT, vmport_cmd_vassert, NULL);
    vmport_register(VMPORT_CMD_VMK_INFO, vmport_cmd_vmk_info, NULL);
    vmport_register(VMPORT_CMD_XPMODE, vmport_cmd_xpmode, NULL);
}

static Property vmport_properties[] = {
    /* Used to enforce compatibility for migration */
    DEFINE_PROP_BIT("x-read-set-eax", VMPortState, compat_flags,
                    VMPORT_COMPAT_READ_SET_EAX_BIT, true),
    DEFINE_PROP_BIT("x-signal-unsupported-cmd", VMPortState, compat_flags,
                    VMPORT_COMPAT_SIGNAL_UNSUPPORTED_CMD_BIT, true),
    DEFINE_PROP_BIT("x-report-vmx-type", VMPortState, compat_flags,
                    VMPORT_COMPAT_REPORT_VMX_TYPE_BIT, true),
    DEFINE_PROP_BIT("x-cmds-v2", VMPortState, compat_flags,
                    VMPORT_COMPAT_CMDS_V2_BIT, true),

    /* Default value taken from open-vm-tools code VERSION_MAGIC definition */
    DEFINE_PROP_UINT32("vmware-vmx-version", VMPortState,
                       vmware_vmx_version, 6),
    /*
     * Value determines which VMware product type host report itself to guest.
     *
     * Most guests are fine with exposing host as VMware ESX server.
     * Some legacy/proprietary guests hard-code a given type.
     *
     * For a complete list of values, refer to enum VMXType at open-vm-tools
     * project (Defined at lib/include/vm_vmx_type.h).
     *
     * Reasonable options:
     * 0 - Unset
     * 1 - VMware Express (deprecated)
     * 2 - VMware ESX Server
     * 3 - VMware Server (Deprecated)
     * 4 - VMware Workstation
     * 5 - ACE 1.x (Deprecated)
     */
    DEFINE_PROP_UINT8("vmware-vmx-type", VMPortState, vmware_vmx_type, 4),

    DEFINE_PROP_END_OF_LIST(),
};

static void vmport_class_initfn(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = vmport_realizefn;
    /* Reason: realize sets global port_state */
    dc->user_creatable = false;
    device_class_set_props(dc, vmport_properties);
}

static const TypeInfo vmport_info = {
    .name          = TYPE_VMPORT,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(VMPortState),
    .class_init    = vmport_class_initfn,
};

static void vmport_register_types(void)
{
    type_register_static(&vmport_info);
}

type_init(vmport_register_types)

#include "kvm/kvm.h"
#include "kvm/kvm-cpu.h"
#include "asm/realm.h"

/*
 * Return codes defined in ARM DEN 0070A
 * ARM DEN 0070A is now merged/consolidated into ARM DEN 0028 C
 */
#define SMCCC_RET_SUCCESS			0
#define SMCCC_RET_NOT_SUPPORTED			-1
#define SMCCC_RET_NOT_REQUIRED			-2
#define SMCCC_RET_INVALID_PARAMETER		-3

static int debug_fd;

void kvm_cpu__set_debug_fd(int fd)
{
	debug_fd = fd;
}

int kvm_cpu__get_debug_fd(void)
{
	return debug_fd;
}

static struct kvm_arm_target *kvm_arm_generic_target;
static struct kvm_arm_target *kvm_arm_targets[KVM_ARM_NUM_TARGETS];

void kvm_cpu__set_kvm_arm_generic_target(struct kvm_arm_target *target)
{
	kvm_arm_generic_target = target;
}

int kvm_cpu__register_kvm_arm_target(struct kvm_arm_target *target)
{
	unsigned int i = 0;

	for (i = 0; i < ARRAY_SIZE(kvm_arm_targets); ++i) {
		if (!kvm_arm_targets[i]) {
			kvm_arm_targets[i] = target;
			return 0;
		}
	}

	return -ENOSPC;
}

struct kvm_cpu *kvm_cpu__arch_init(struct kvm *kvm, unsigned long cpu_id)
{
	struct kvm_arm_target *target = NULL;
	struct kvm_cpu *vcpu;
	int coalesced_offset, mmap_size, err = -1;
	unsigned int i;
	struct kvm_vcpu_init preferred_init;
	struct kvm_vcpu_init vcpu_init = {
		.features = {},
	};

	vcpu = calloc(1, sizeof(struct kvm_cpu));
	if (!vcpu)
		return NULL;

	vcpu->vcpu_fd = ioctl(kvm->vm_fd, KVM_CREATE_VCPU, cpu_id);
	if (vcpu->vcpu_fd < 0)
		die_perror("KVM_CREATE_VCPU ioctl");

	mmap_size = ioctl(kvm->sys_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
	if (mmap_size < 0)
		die_perror("KVM_GET_VCPU_MMAP_SIZE ioctl");

	vcpu->kvm_run = mmap(NULL, mmap_size, PROT_RW, MAP_SHARED,
			     vcpu->vcpu_fd, 0);
	if (vcpu->kvm_run == MAP_FAILED)
		die("unable to mmap vcpu fd");

	/* VCPU 0 is the boot CPU, the others start in a poweroff state. */
	if (cpu_id > 0)
		vcpu_init.features[0] |= (1UL << KVM_ARM_VCPU_POWER_OFF);

	/* Set KVM_ARM_VCPU_PSCI_0_2 if available */
	if (kvm__supports_extension(kvm, KVM_CAP_ARM_PSCI_0_2)) {
		vcpu_init.features[0] |= (1UL << KVM_ARM_VCPU_PSCI_0_2);
	}

	kvm_cpu__select_features(kvm, &vcpu_init);

	/*
	 * If the preferred target ioctl is successful then
	 * use preferred target else try each and every target type
	 */
	err = ioctl(kvm->vm_fd, KVM_ARM_PREFERRED_TARGET, &preferred_init);
	if (!err) {
		/* Match preferred target CPU type. */
		for (i = 0; i < ARRAY_SIZE(kvm_arm_targets); ++i) {
			if (!kvm_arm_targets[i])
				continue;
			if (kvm_arm_targets[i]->id == preferred_init.target) {
				target = kvm_arm_targets[i];
				break;
			}
		}
		if (!target) {
			target = kvm_arm_generic_target;
			vcpu_init.target = preferred_init.target;
		} else {
			vcpu_init.target = target->id;
		}
		err = ioctl(vcpu->vcpu_fd, KVM_ARM_VCPU_INIT, &vcpu_init);
	} else {
		/* Find an appropriate target CPU type. */
		for (i = 0; i < ARRAY_SIZE(kvm_arm_targets); ++i) {
			if (!kvm_arm_targets[i])
				continue;
			target = kvm_arm_targets[i];
			vcpu_init.target = target->id;
			err = ioctl(vcpu->vcpu_fd, KVM_ARM_VCPU_INIT, &vcpu_init);
			if (!err)
				break;
		}
		if (err)
			die("Unable to find matching target");
	}

	/* Populate the vcpu structure. */
	vcpu->kvm		= kvm;
	vcpu->cpu_id		= cpu_id;
	vcpu->cpu_type		= vcpu_init.target;
	vcpu->cpu_compatible	= target->compatible;
	vcpu->is_running	= true;

	if (err || target->init(vcpu))
		die("Unable to initialise vcpu");

	coalesced_offset = ioctl(kvm->sys_fd, KVM_CHECK_EXTENSION,
				 KVM_CAP_COALESCED_MMIO);
	if (coalesced_offset)
		vcpu->ring = (void *)vcpu->kvm_run +
			     (coalesced_offset * PAGE_SIZE);

	if (kvm_cpu__configure_features(vcpu))
		die("Unable to configure requested vcpu features");

	return vcpu;
}

void kvm_cpu__arch_nmi(struct kvm_cpu *cpu)
{
}

void kvm_cpu__delete(struct kvm_cpu *vcpu)
{
	kvm_cpu__teardown_pvtime(vcpu->kvm);
	free(vcpu);
}

static bool handle_mem_share(struct kvm_cpu *vcpu)
{
	u64 gpa = vcpu->kvm_run->hypercall.args[0];

	vcpu->kvm_run->hypercall.ret = SMCCC_RET_SUCCESS;

	if (!vcpu->kvm->cfg.pkvm) {
		pr_warning("%s: non-protected guest memshare request for gpa 0x%llx",
			   __func__, gpa);
		return true;
	}

	set_guest_memory_attributes(vcpu->kvm, gpa, PAGE_SIZE, 0);
	map_guest_range(vcpu->kvm, gpa, PAGE_SIZE);

	return true;
}

static bool handle_mem_unshare(struct kvm_cpu *vcpu)
{
	u64 gpa = vcpu->kvm_run->hypercall.args[0];

	vcpu->kvm_run->hypercall.ret = SMCCC_RET_SUCCESS;

	if (!vcpu->kvm->cfg.pkvm) {
		pr_warning("%s: non-protected guest memunshare request for gpa 0x%llx",
			   __func__, gpa);
		return true;
	}

	unmap_guest_range(vcpu->kvm, gpa, PAGE_SIZE);
	set_guest_memory_attributes(vcpu->kvm, gpa, PAGE_SIZE, KVM_MEMORY_ATTRIBUTE_PRIVATE);

	return true;
}

static bool handle_hypercall(struct kvm_cpu *vcpu)
{
	u64 call_nr = vcpu->kvm_run->hypercall.nr;

	switch (call_nr)
	{
	case ARM_SMCCC_KVM_FUNC_MEM_SHARE:
		return handle_mem_share(vcpu);
	case ARM_SMCCC_KVM_FUNC_MEM_UNSHARE:
		return handle_mem_unshare(vcpu);
	default:
		pr_warning("%s: Unhandled exit hypercall: 0x%llx, 0x%llx, 0x%llx, 0x%llx",
			   __func__,
			   vcpu->kvm_run->hypercall.nr,
			   vcpu->kvm_run->hypercall.ret,
			   vcpu->kvm_run->hypercall.args[0],
			   vcpu->kvm_run->hypercall.args[1]);
		break;
	}

	return true;
}

static bool handle_memoryfault(struct kvm_cpu *vcpu)
{
	u64 flags = vcpu->kvm_run->memory_fault.flags;
	u64 gpa = vcpu->kvm_run->memory_fault.gpa;
	u64 size = vcpu->kvm_run->memory_fault.size;

	if (flags & KVM_MEMORY_EXIT_FLAG_PRIVATE) {
		unmap_guest_range(vcpu->kvm, gpa, size);
		set_guest_memory_attributes(vcpu->kvm, gpa, size,
					    KVM_MEMORY_ATTRIBUTE_PRIVATE);
	} else {
		set_guest_memory_attributes(vcpu->kvm, gpa, size, 0);
		map_guest_range(vcpu->kvm, gpa, size);
	}

	return true;
}

bool kvm_cpu__handle_exit(struct kvm_cpu *vcpu)
{
	switch (vcpu->kvm_run->exit_reason) {
	case KVM_EXIT_HYPERCALL:
		return handle_hypercall(vcpu);
	case KVM_EXIT_MEMORY_FAULT:
		return handle_memoryfault(vcpu);
	}

	return false;
}

void kvm_cpu__show_page_tables(struct kvm_cpu *vcpu)
{
}

void kvm_cpu__arch_unhandled_mmio(struct kvm_cpu *vcpu)
{
	struct kvm_vcpu_events events = { };

	if (!kvm__is_realm(vcpu->kvm))
		return;

	events.exception.ext_dabt_pending = 1;

	if (ioctl(vcpu->vcpu_fd, KVM_SET_VCPU_EVENTS, &events) < 0)
		die_perror("KVM_SET_VCPU_EVENTS failed");
}

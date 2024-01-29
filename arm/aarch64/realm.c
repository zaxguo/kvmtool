#include "kvm/kvm.h"

#include "asm/realm.h"

static void realm_configure_hash_algo(struct kvm *kvm)
{
	struct kvm_cap_arm_rme_config_item hash_algo_cfg = {
		.cfg	= KVM_CAP_ARM_RME_CFG_HASH_ALGO,
		.hash_algo = kvm->arch.measurement_algo,
	};

	struct kvm_enable_cap rme_config = {
		.cap = KVM_CAP_ARM_RME,
		.args[0] = KVM_CAP_ARM_RME_CONFIG_REALM,
		.args[1] = (u64)&hash_algo_cfg,
	};

	if (ioctl(kvm->vm_fd, KVM_ENABLE_CAP, &rme_config) < 0)
		die_perror("KVM_CAP_RME(KVM_CAP_ARM_RME_CONFIG_REALM) hash_algo");
}

static void realm_configure_parameters(struct kvm *kvm)
{
	realm_configure_hash_algo(kvm);
}

static void kvm_arm_realm_create_realm_descriptor(struct kvm *kvm)
{
	struct kvm_enable_cap rme_create_rd = {
		.cap = KVM_CAP_ARM_RME,
		.args[0] = KVM_CAP_ARM_RME_CREATE_RD,
	};

	realm_configure_parameters(kvm);
	if (ioctl(kvm->vm_fd, KVM_ENABLE_CAP, &rme_create_rd) < 0)
		die_perror("KVM_CAP_RME(KVM_CAP_ARM_RME_CREATE_RD)");
}

static int kvm_arm_realm_finalize(struct kvm *kvm)
{
	if (!kvm__is_realm(kvm))
		return 0;

	kvm_arm_realm_create_realm_descriptor(kvm);

	return 0;
}
last_init(kvm_arm_realm_finalize)

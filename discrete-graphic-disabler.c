/**
 *
 * Disable discrete graphics (currently Nvidia only)
 *
 * Base on Bumblebee bbswitch
 *
 * Usage:
 * Disable discrete graphics when loaded
 * Restore the former state when unloaded
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/pci.h>
#include <linux/acpi.h>
#include <linux/module.h>
#include <asm/uaccess.h>
#include <linux/suspend.h>
#include <linux/pm_runtime.h>

#define DISCRETE_DISABLER_VERSION "0.2"

enum {
	GPU_UNCHANGED = -1,
	GPU_DISABLE = 0,
	GPU_ENABLE = 1
};

static int device_vendor = 0xffff;
static int discrete_state = GPU_UNCHANGED;
static int load_discrete_state = GPU_UNCHANGED;

/*
  The next UUID has been found as well in
  https://bugs.launchpad.net/lpbugreporter/+bug/752542:

  0xD3, 0x73, 0xD8, 0x7E, 0xD0, 0xC2, 0x4F, 0x4E,
  0xA8, 0x54, 0x0F, 0x13, 0x17, 0xB0, 0x1C, 0x2C 
  It looks like something for Intel GPU:
  http://lxr.linux.no/#linux+v3.1.5/drivers/gpu/drm/i915/intel_acpi.c
*/

#define DSM_TYPE_UNSUPPORTED    0
#define DSM_TYPE_OPTIMUS        1
#define DSM_TYPE_NVIDIA         2
static struct pci_dev *dis_dev;
static acpi_handle dis_handle;

/* used for keeping the PM event handler */
static struct notifier_block nb;

// Return GPU_DISABLE if disabled, GPU_ENABLE if enabled
static int get_discrete_state(void) {
	u32 cfg_word;
	/* read first config word which contains Vendor and Device ID, 
	 * if all bits are 1, the device is assuemd to be off, 
	 * if one of the bits is not 1, the device is on.
	 */
	pci_read_config_dword(dis_dev, 0, &cfg_word);
	
	if(likely(~cfg_word))
		return GPU_ENABLE;
	else
		return GPU_DISABLE;
}


/* power bus so we can read PCI configuration space */
static void dis_dev_get(void) {
	if (dis_dev->bus && dis_dev->bus->self)
		pm_runtime_get_sync(&dis_dev->bus->self->dev);
}

static void dis_dev_put(void) {
	if (dis_dev->bus && dis_dev->bus->self)
		pm_runtime_put_sync(&dis_dev->bus->self->dev);
}


static void discrete_off(void) {
	struct acpi_object_list arg;

	pr_info("turning discrete off\n");
	if (unlikely(get_discrete_state() == GPU_DISABLE)) {
		return;
	}

	// to prevent the system from possibly locking up, don't disable the device
	// if it's still in use by a driver (i.e. nouveau or nvidia)
	if(dis_dev->driver) {
		pr_warn("device %s is in use by driver '%s', refusing OFF\n",
			dev_name(&dis_dev->dev), dis_dev->driver->name);
		return;
	}

	pr_info("disabling discrete grapics\n");

	arg.count = 0;
	arg.pointer = NULL;
	acpi_evaluate_object(dis_handle, "_OFF", &arg, NULL);

	//pci_save_state(dis_dev);
	//pci_clear_master(dis_dev);
	
	//pci_disable_device(dis_dev);
	//pci_set_power_state(dis_dev, PCI_D3cold);

	discrete_state = GPU_DISABLE;
}


static void discrete_on(void) {
	struct acpi_object_list arg;

	pr_info("turn discrete on\n");
	if (unlikely(get_discrete_state() == GPU_ENABLE))
		return;

	pr_info("enabling discrete graphics\n");

	arg.count = 0;
	arg.pointer = NULL;
	acpi_evaluate_object(dis_handle, "_ON", &arg, NULL);

	discrete_state = GPU_ENABLE;
}


static int discrete_pm_handler(struct notifier_block *nbp,
			      unsigned long event_type, void *p) {
	switch (event_type) {
	case PM_HIBERNATION_PREPARE:
	case PM_SUSPEND_PREPARE:
		// enable the device before suspend to avoid the PCI config space from
		// being saved incorrectly
		if (load_discrete_state == GPU_ENABLE &&
		    discrete_state == GPU_DISABLE) {
			dis_dev_get();
			discrete_on();
			dis_dev_put();
		}
		break;
	case PM_POST_HIBERNATION:
	case PM_POST_SUSPEND:
	case PM_POST_RESTORE:
		// after suspend, the card is on, but if it was off before suspend,
		// disable it again
		if (load_discrete_state == GPU_ENABLE &&
		    discrete_state == GPU_ENABLE) {
			dis_dev_get();
			discrete_off();
			dis_dev_put();
		}
		break;
	case PM_RESTORE_PREPARE:
		// deliberately don't do anything as it does not occur before suspend
		// nor hibernate, but before restoring a saved image. In that case,
		// either PM_POST_HIBERNATION or PM_POST_RESTORE will be called
		break;
	}
	return 0;
}
	   
static int __init discrete_disabler_init(void)
{
	struct pci_dev *pdev = NULL;
	struct pci_dev *igd_dev = NULL;
	acpi_handle igd_handle = NULL;

	pr_info("version %s\n", DISCRETE_DISABLER_VERSION);

	while ((pdev = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, pdev)) != NULL) {
		acpi_handle handle;
		struct acpi_buffer buf = { ACPI_ALLOCATE_BUFFER, NULL};
		int pci_class = pdev->class >> 8;

		if (pci_class != PCI_CLASS_DISPLAY_VGA && 
			pci_class != PCI_CLASS_DISPLAY_3D &&
			pci_class != PCI_CLASS_DISPLAY_OTHER){
			
			continue;
		}

		handle = DEVICE_ACPI_HANDLE(&pdev->dev);
		
		if (!handle) {
			pr_warn("can not find ACPI handle for VGA device %s\n",
				dev_name(&pdev->dev));
			continue;
		}

		acpi_get_name(handle, ACPI_FULL_PATHNAME, &buf);

		if (pdev->vendor == PCI_VENDOR_ID_INTEL) {
			igd_dev = pdev;
			igd_handle = handle;
			pr_info("Found intergrated VGA device %s: %s\n",
				dev_name(&pdev->dev), (char *)buf.pointer);
		} else {
			dis_dev = pdev;
			dis_handle = handle;
			pr_info("Found discrete VGA device %s: %s\n",
				dev_name(&pdev->dev), (char *)buf.pointer);
		}

		kfree(buf.pointer);

		// If both intergrated and discrete VGA devices are found, we can break now.
		if(unlikely(igd_handle && dis_handle)) {
			break;
		}
	}
	
	if (!dis_dev) {
		pr_err("No discrete VGA device found!\n");
		return -ENODEV;
	}

	device_vendor = dis_dev->vendor;


	pr_info("Successfully loaded. Discrete card found: %s\n",
		dev_name(&dis_dev->dev));

	dis_dev_get();
	load_discrete_state = get_discrete_state();
	
	if (unlikely(load_discrete_state == GPU_DISABLE)) {
		pr_info("Discrete card %s is off. Do nothing.",
			dev_name(&dis_dev->dev));
		goto nothing;
	} else {
		discrete_off();
		pr_info("Successfully disable discrete card %s\n",
			dev_name(&dis_dev->dev));
	}
	
	nb.notifier_call = &discrete_pm_handler;
	register_pm_notifier(&nb);

nothing:dis_dev_put();
	return 0;
}

static void __exit discrete_disabler_exit(void)
{
	if (!dis_dev || !dis_handle) {
		return;
	}

	if (unlikely(load_discrete_state == GPU_DISABLE)) {
		pr_info("discrete card %s is disabled before module load. Do nothing.\n",
			dev_name(&dis_dev->dev));
		return;
	}

	dis_dev_get();
	
	if (discrete_state == GPU_ENABLE) {
		pr_info("discrete card %s has already been enabled. Do nothing.\n",
			dev_name(&dis_dev->dev));
		return;
	}

	discrete_on();

	pr_info("Unloaded and Discrete card %s is enabled.\n",
		dev_name(&dis_dev->dev));
	
	dis_dev_put();
	
	if (nb.notifier_call)
		unregister_pm_notifier(&nb);
}

module_init(discrete_disabler_init);
module_exit(discrete_disabler_exit);

static struct pci_device_id discrete_pci_table[] = {
	{
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_ANY_ID),
		.class = PCI_BASE_CLASS_DISPLAY << 16,
		.class_mask  = 0xff << 16,
	},
	{
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA_SGS, PCI_ANY_ID),
		.class = PCI_BASE_CLASS_DISPLAY << 16,
		.class_mask  = 0xff << 16,
	},
	{
		PCI_DEVICE(PCI_VENDOR_ID_ATI, PCI_ANY_ID),
		.class = PCI_BASE_CLASS_DISPLAY << 16,
		.class_mask  = 0xff << 16,
	},
	{}
};

MODULE_DEVICE_TABLE(pci, discrete_pci_table);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Yang Bai <hamo@canonical.com>");
MODULE_AUTHOR("Shuduo Sang <shuduo.sang@canonical.com>");
MODULE_VERSION(DISCRETE_DISABLER_VERSION);


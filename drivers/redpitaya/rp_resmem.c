// SPDX-License-Identifier: GPL-2.0
/*
 * Red Pitaya reserved memory access
 *
 * Gives user space a mapping of a reserved-memory region that the FPGA DMA
 * engines read and write, without /dev/mem.
 *
 * When the kernel has struct pages for the region (the reserved-memory node
 * has no "no-map"), the region is mapped page by page with vm_insert_pages().
 * Such a mapping can be pinned by get_user_pages(), so the data can go to a
 * socket with MSG_ZEROCOPY: the network controller reads it straight from the
 * region and the CPU does not copy it. A /dev/mem mapping is VM_PFNMAP and
 * cannot be pinned. For a "no-map" region the driver falls back to
 * remap_pfn_range(): the mapping works, zero copy does not.
 *
 * The mapping is uncached, as with /dev/mem and O_SYNC: the FPGA writes the
 * memory behind the CPU caches.
 *
 * The pages of a reserved region are never freed, so a page still held by the
 * network stack after munmap() or after the driver is unbound stays valid.
 * Reusing the memory too early is the user's concern: with MSG_ZEROCOPY a
 * buffer may be written again only after its completion was read from the
 * socket error queue.
 *
 * Device tree:
 *
 *	reserved-memory {
 *		dma_region: buffer@2000000_b {
 *			reg = <0x2000000 0x2000000>;
 *		};
 *	};
 *
 *	rp-resmem {
 *		compatible = "redpitaya,rp-resmem";
 *		memory-region = <&dma_region>;
 *		label = "rp_resmem";	(optional, the /dev name; default rp_resmem)
 *	};
 *
 * User space: /dev/<label>, mmap() with MAP_SHARED, the offset is relative to
 * the start of the region. sysfs /sys/class/misc/<label>/: base, size (hex)
 * and zerocopy (1 when the mapping can be pinned).
 */

#include <linux/device.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>

#define RP_RESMEM_DEFAULT_NAME	"rp_resmem"
#define RP_RESMEM_BATCH		64

struct rp_resmem {
	struct miscdevice misc;
	phys_addr_t base;
	phys_addr_t size;
	bool has_pages;
};

static struct rp_resmem *to_rp_resmem(struct miscdevice *misc)
{
	return container_of(misc, struct rp_resmem, misc);
}

static int rp_resmem_insert_pages(struct vm_area_struct *vma, unsigned long first_pfn,
				  unsigned long npages)
{
	struct page *pages[RP_RESMEM_BATCH];
	unsigned long addr = vma->vm_start;
	unsigned long done = 0;

	while (done < npages) {
		unsigned long n = min_t(unsigned long, npages - done, RP_RESMEM_BATCH);
		unsigned long left = n;
		unsigned long i;
		int ret;

		for (i = 0; i < n; i++)
			pages[i] = pfn_to_page(first_pfn + done + i);
		ret = vm_insert_pages(vma, addr, pages, &left);
		if (ret)
			return ret;
		if (left)
			return -EFAULT;
		addr += n << PAGE_SHIFT;
		done += n;
	}
	return 0;
}

static int rp_resmem_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct rp_resmem *rm = to_rp_resmem(file->private_data);
	unsigned long len = vma->vm_end - vma->vm_start;
	unsigned long region_pages = rm->size >> PAGE_SHIFT;
	unsigned long first_pfn;

	/* A private mapping would turn writes into copy-on-write anonymous pages */
	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;
	if (vma->vm_pgoff >= region_pages || (len >> PAGE_SHIFT) > region_pages - vma->vm_pgoff)
		return -EINVAL;

	first_pfn = PHYS_PFN(rm->base) + vma->vm_pgoff;
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	if (!rm->has_pages)
		return remap_pfn_range(vma, vma->vm_start, first_pfn, len, vma->vm_page_prot);

	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
	return rp_resmem_insert_pages(vma, first_pfn, len >> PAGE_SHIFT);
}

static const struct file_operations rp_resmem_fops = {
	.owner = THIS_MODULE,
	.mmap = rp_resmem_mmap,
};

static ssize_t base_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct rp_resmem *rm = to_rp_resmem(dev_get_drvdata(dev));

	return sysfs_emit(buf, "0x%llx\n", (unsigned long long)rm->base);
}
static DEVICE_ATTR_RO(base);

static ssize_t size_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct rp_resmem *rm = to_rp_resmem(dev_get_drvdata(dev));

	return sysfs_emit(buf, "0x%llx\n", (unsigned long long)rm->size);
}
static DEVICE_ATTR_RO(size);

static ssize_t zerocopy_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct rp_resmem *rm = to_rp_resmem(dev_get_drvdata(dev));

	return sysfs_emit(buf, "%d\n", rm->has_pages);
}
static DEVICE_ATTR_RO(zerocopy);

static struct attribute *rp_resmem_attrs[] = {
	&dev_attr_base.attr,
	&dev_attr_size.attr,
	&dev_attr_zerocopy.attr,
	NULL,
};
ATTRIBUTE_GROUPS(rp_resmem);

static bool rp_resmem_pages_valid(phys_addr_t base, phys_addr_t size)
{
	unsigned long pfn = PHYS_PFN(base);
	unsigned long end = PHYS_PFN(base + size);

	for (; pfn < end; pfn++)
		if (!pfn_valid(pfn))
			return false;
	return true;
}

static int rp_resmem_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *region;
	struct reserved_mem *rmem;
	struct rp_resmem *rm;
	const char *name = RP_RESMEM_DEFAULT_NAME;
	int ret;

	region = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!region)
		return dev_err_probe(dev, -EINVAL, "no memory-region\n");
	/* Only a reserved-memory region: never hand out memory the kernel uses */
	rmem = of_reserved_mem_lookup(region);
	of_node_put(region);
	if (!rmem || !rmem->size)
		return dev_err_probe(dev, -EINVAL,
				     "memory-region is not a reserved-memory region\n");
	if (!PAGE_ALIGNED(rmem->base) || !PAGE_ALIGNED(rmem->size))
		return dev_err_probe(dev, -EINVAL, "region %pa size %pa is not page aligned\n",
				     &rmem->base, &rmem->size);

	rm = devm_kzalloc(dev, sizeof(*rm), GFP_KERNEL);
	if (!rm)
		return -ENOMEM;
	rm->base = rmem->base;
	rm->size = rmem->size;
	rm->has_pages = rp_resmem_pages_valid(rm->base, rm->size);

	of_property_read_string(dev->of_node, "label", &name);
	rm->misc.name = devm_kstrdup(dev, name, GFP_KERNEL);
	if (!rm->misc.name)
		return -ENOMEM;
	rm->misc.minor = MISC_DYNAMIC_MINOR;
	rm->misc.fops = &rp_resmem_fops;
	rm->misc.parent = dev;
	rm->misc.groups = rp_resmem_groups;
	rm->misc.mode = 0600;

	ret = misc_register(&rm->misc);
	if (ret)
		return dev_err_probe(dev, ret, "misc_register\n");
	platform_set_drvdata(pdev, rm);

	dev_info(dev, "/dev/%s: %pa size %pa, %s\n", rm->misc.name, &rm->base, &rm->size,
		 rm->has_pages ? "page mapped, zero copy" :
				 "no struct pages (no-map), no zero copy");
	return 0;
}

static void rp_resmem_remove(struct platform_device *pdev)
{
	struct rp_resmem *rm = platform_get_drvdata(pdev);

	misc_deregister(&rm->misc);
}

static const struct of_device_id rp_resmem_of_match[] = {
	{ .compatible = "redpitaya,rp-resmem" },
	{ }
};
MODULE_DEVICE_TABLE(of, rp_resmem_of_match);

static struct platform_driver rp_resmem_driver = {
	.probe = rp_resmem_probe,
	.remove = rp_resmem_remove,
	.driver = {
		.name = "rp-resmem",
		.of_match_table = rp_resmem_of_match,
	},
};
module_platform_driver(rp_resmem_driver);

MODULE_AUTHOR("Red Pitaya");
MODULE_DESCRIPTION("Red Pitaya reserved memory access for user space and zero-copy send");
MODULE_LICENSE("GPL");
